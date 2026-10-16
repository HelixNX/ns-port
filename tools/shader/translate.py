"""Maxwell shader -> GLSL 4.60 for Vulkan (our own translator; see CLAUDE.md, decision 4).

Usage: python translate.py [--check | --spv] [shader dir] [out dir]
    default: ../../../build/shaders -> ../../../build/shaders/glsl (<hash>.vert / .frag / .comp)
    --check  also compiles every output with glslang (deps/glslang/bin/glslang.exe) and reports failures
    --spv    the same, keeping the SPIR-V in <shader dir>/spv/<hash>.spv (what the runtime loads for now)

Model:
  - Registers are `uint rN` (bit patterns; floats through F()/U()), predicates `bool pN`, condition codes cc_z/s/c/o.
  - Control flow: GLSLC code here has no loops (every backward branch is the trailing `BRA self`), so the CFG is a
    DAG in address order. Each basic block runs under a `reached` flag that its predecessors set; SSY/SYNC are
    resolved statically (the reconvergence stack is the same on every path into a block).
  - Constant buffers c[i] are uniform blocks at set 0, binding stage_base + i, where i follows NVN's layout:
    c[0] driver data (storage buffer table at 0x110, 16 bytes per slot), c[1] GLSLC's embedded constants,
    c[2] texture handles (bound slot k at 0x20 + 8k), c[3 + b] the game's uniform buffer binding b.
    The executor (runtime/gpu.c) fills c[0] and c[2] itself.
  - Textures are bindless: the 32-bit NVN handle (texture id | sampler id << 20) indexes the texture pool's
    descriptor array (set 1) and the sampler pool's (set 2).
  - Global memory (LDG/STG) uses 64-bit device addresses, which c[0] holds instead of guest addresses.
  - Vertex attributes are vec4 inputs at location = attribute index; the runtime may define A<k>_INT / A<k>_UINT
    for integer formats (see the attribute macros in the preamble).
"""
import os
import struct
import subprocess
import sys

from maxwell import Shader, opcode, bits

RZ = 255
PT = 7
STAGE_EXT = {'vertex': 'vert', 'fragment': 'frag', 'compute': 'comp', 'geometry': 'geom',
             'tess_control': 'tesc', 'tess_eval': 'tese'}
STAGE_BASE = {'vertex': 0, 'fragment': 16, 'compute': 0, 'geometry': 32, 'tess_control': 48, 'tess_eval': 64}


class Unsupported(Exception):
    pass


def sext(v, n):
    return v - (1 << n) if v >> (n - 1) & 1 else v


def flit(u):
    """an exact float literal from its bits"""
    u &= 0xffffffff
    f = struct.unpack('<f', struct.pack('<I', u))[0]
    if f == f and abs(f) != float('inf') and struct.unpack('<I', struct.pack('<f', float(repr(f))))[0] == u and \
            (f == 0 or 1e-30 < abs(f) < 1e30) and not (u == 0x80000000):
        s = repr(f)
        if 'e' not in s and '.' not in s:
            s += '.0'
        return s
    return 'uintBitsToFloat(0x%08xu)' % u


class Translator:
    def __init__(self, sh):
        self.sh = sh
        self.stage = sh.stage
        self.lines = []
        self.regs = set()
        self.cbufs = set()
        self.in_attrs = {}          # generic input index -> interpolation ('smooth', 'flat', 'noperspective')
        self.out_attrs = set()
        self.uses = set()           # features: 'global', 'lmem', 'fragdepth', 'vertexid', ...
        self.indent = 1
        self.lmem_words = 0
        sph = sh.sph
        if sph:
            self.sph_words = struct.unpack('<20I', sph)
        if self.stage == 'fragment':
            self.omap_target, self.omap_flags = struct.unpack_from('<II', sph, 0x48)

    # ------------------------------------------------------------------ emit helpers
    def emit(self, s):
        self.lines.append('    ' * self.indent + s)

    def R(self, r):
        """register as uint"""
        if r == RZ:
            return '0u'
        self.regs.add(r)
        return 'r%d' % r

    def Rf(self, r):
        if r == RZ:
            return '0.0'
        self.regs.add(r)
        return 'F(r%d)' % r

    def set_r(self, r, expr):
        if r != RZ:
            self.regs.add(r)
            self.emit('r%d = %s;' % (r, expr))

    def set_rf(self, r, expr):
        if r != RZ:
            self.regs.add(r)
            self.emit('r%d = U(%s);' % (r, expr))

    def P(self, p, neg=False):
        if p == PT:
            return 'false' if neg else 'true'
        return ('!p%d' % p) if neg else ('p%d' % p)

    def set_p(self, p, expr):
        if p != PT:
            self.emit('p%d = %s;' % (p, expr))

    def cbuf(self, idx, off):
        """32-bit word of c[idx] at byte offset off (constant)"""
        self.cbufs.add(idx)
        if off & 3:
            raise Unsupported('unaligned cbuf offset')
        return 'cb%d.d[%d][%d]' % (idx, off >> 4, (off >> 2) & 3)

    def cbuf_dyn(self, idx, expr):
        """32-bit word of c[idx] at a byte offset computed at run time"""
        self.cbufs.add(idx)
        self.uses.add('cbdyn%d' % idx)
        return 'cbr%d(%s)' % (idx, expr)

    # operand B by instruction form ('R' gpr20, 'C' cbuf, 'I' imm20, '32I' imm32)
    def op_b_u(self, w, form):
        if form == 'R':
            return self.R(bits(w, 20, 8))
        if form == 'C':
            return self.cbuf(bits(w, 34, 5), bits(w, 20, 14) * 4)
        if form == 'I':
            v = bits(w, 20, 19) | bits(w, 56, 1) << 19
            return '0x%08xu' % (sext(v, 20) & 0xffffffff)
        if form == '32I':
            return '0x%08xu' % bits(w, 20, 32)
        raise Unsupported(form)

    def op_b_f(self, w, form):
        if form == 'R':
            return self.Rf(bits(w, 20, 8))
        if form == 'C':
            return 'F(%s)' % self.cbuf(bits(w, 34, 5), bits(w, 20, 14) * 4)
        if form == 'I':
            return flit(bits(w, 20, 19) << 12 | bits(w, 56, 1) << 31)
        if form == '32I':
            return flit(bits(w, 20, 32))
        raise Unsupported(form)

    @staticmethod
    def fmod(x, neg, ab):
        if ab:
            x = 'abs(%s)' % x
        if neg:
            x = '(-%s)' % x
        return x

    @staticmethod
    def sat(x, s):
        return 'clamp(%s, 0.0, 1.0)' % x if s else x

    # ------------------------------------------------------------------ comparisons
    @staticmethod
    def fcmp(op, a, b):
        """FPCompareOp; the unordered forms are true when either side is NaN"""
        n = '(isnan(%s) || isnan(%s))' % (a, b)
        return {
            0: 'false', 1: '(%s < %s)' % (a, b), 2: '(%s == %s)' % (a, b), 3: '(%s <= %s)' % (a, b),
            4: '(%s > %s)' % (a, b), 5: '(%s < %s || %s > %s)' % (a, b, a, b), 6: '(%s >= %s)' % (a, b),
            7: '!%s' % n, 8: n, 9: '!(%s >= %s)' % (a, b), 10: '!(%s < %s || %s > %s)' % (a, b, a, b),
            11: '!(%s > %s)' % (a, b), 12: '!(%s <= %s)' % (a, b), 13: '(%s != %s)' % (a, b),
            14: '!(%s < %s)' % (a, b), 15: 'true',
        }[op]

    @staticmethod
    def icmp(op, a, b, signed):
        if op in (0, 7):
            return 'false' if op == 0 else 'true'
        if signed:
            a, b = 'int(%s)' % a, 'int(%s)' % b
        return '(%s %s %s)' % (a, {1: '<', 2: '==', 3: '<=', 4: '>', 5: '!=', 6: '>='}[op], b)

    @staticmethod
    def bop(op, a, b):
        if op == 0:
            return '(%s && %s)' % (a, b)
        if op == 1:
            return '(%s || %s)' % (a, b)
        if op == 2:
            return '(%s != %s)' % (a, b)
        raise Unsupported('bop %d' % op)

    def flow(self, test):
        """condition-code test (FlowTest)"""
        t = {0: 'false', 15: 'true', 13: '!cc_z', 5: '!cc_z', 2: 'cc_z', 10: 'cc_z',
             1: '(cc_s != cc_o)', 6: '(cc_s == cc_o)', 4: '(!cc_z && cc_s == cc_o)', 3: '(cc_z || cc_s != cc_o)'}
        if test not in t:
            raise Unsupported('flow test %d' % test)
        if test not in (0, 15):
            self.uses.add('cc')
        return t[test]

    # ------------------------------------------------------------------ attributes
    def attr_in(self, off):
        """input attribute word (vertex inputs, or fragment inputs for IPA)"""
        if 0x80 <= off < 0x280:
            k, c = (off - 0x80) >> 4, (off >> 2) & 3
            if self.stage == 'vertex':
                self.in_attrs.setdefault(k, 'smooth')
                return 'U(in_a%d[%d])' % (k, c), True
            self.in_attrs.setdefault(k, self.interp_of(k, c))
            return 'U(in_a%d[%d])' % (k, c), True
        if self.stage == 'fragment':
            m = {0x70: 'U(gl_FragCoord.x)', 0x74: 'U(gl_FragCoord.y)', 0x78: 'U(gl_FragCoord.z)',
                 0x7c: 'U(gl_FragCoord.w)', 0x3fc: '(gl_FrontFacing ? 0xffffffffu : 0u)',
                 0x2e0: 'U(gl_PointCoord.x)', 0x2e4: 'U(gl_PointCoord.y)'}
            if off in m:
                return m[off], False
        if self.stage == 'vertex':
            if off == 0x2fc:
                return 'uint(gl_VertexIndex)', False
            if off == 0x2f8:
                return 'uint(gl_InstanceIndex - gl_BaseInstance)', False
        raise Unsupported('input attribute %#x' % off)

    def interp_of(self, k, c):
        mode = self.sh.sph[0x18 + k] >> (2 * c) & 3
        return {0: 'smooth', 1: 'flat', 2: 'smooth', 3: 'noperspective'}[mode]

    def attr_out(self, off, val):
        if 0x80 <= off < 0x280:
            k, c = (off - 0x80) >> 4, (off >> 2) & 3
            self.out_attrs.add(k)
            self.emit('out_a%d[%d] = F(%s);' % (k, c, val))
            return
        m = {0x70: 'gl_Position.x', 0x74: 'gl_Position.y', 0x78: 'gl_Position.z', 0x7c: 'gl_Position.w',
             0x6c: 'gl_PointSize'}
        if off in m:
            self.emit('%s = F(%s);' % (m[off], val))
            return
        if 0x2c0 <= off < 0x2e0:
            self.uses.add('clip')
            self.emit('gl_ClipDistance[%d] = F(%s);' % ((off - 0x2c0) >> 2, val))
            return
        if off == 0x64:
            self.emit('gl_Layer = int(%s);' % val)
            return
        if off == 0x68:
            self.emit('gl_ViewportIndex = int(%s);' % val)
            return
        raise Unsupported('output attribute %#x' % off)

    # ------------------------------------------------------------------ textures
    def tex_handle(self, cbuf_offset):
        return self.cbuf(2, cbuf_offset * 4)

    def sample(self, kind, handle, coords, lod=None, bias=None, dref=None, offset=None, gather=None, query=False):
        """GLSL sampling expression; kind: 1D 2D 3D CUBE 2DARRAY CUBEARRAY 1DARRAY. Returns vec4 (or float for dref)"""
        arr = {'1D': 't1d', '2D': 't2d', '3D': 't3d', 'CUBE': 'tcube', '2DARRAY': 't2da', 'CUBEARRAY': 'tcubea',
               '1DARRAY': 't1da'}[kind]
        styp = {'1D': 'sampler1D', '2D': 'sampler2D', '3D': 'sampler3D', 'CUBE': 'samplerCube',
                '2DARRAY': 'sampler2DArray', 'CUBEARRAY': 'samplerCubeArray', '1DARRAY': 'sampler1DArray'}[kind]
        if dref is not None:
            styp += 'Shadow'
        self.uses.add('tex_' + arr)
        s = '%s(%s[nonuniformEXT(min(%s & 0xfffffu, 65535u))], smp[nonuniformEXT(min(%s >> 20, 4095u))])' % (
            styp, arr, handle, handle)
        if query:
            return 'textureQueryLod(%s, %s)' % (s, coords)
        if gather is not None:
            if dref is not None:
                return 'textureGather(%s, %s, %s)' % (s, coords, dref)
            if offset:
                return 'textureGatherOffset(%s, %s, %s, %d)' % (s, coords, offset, gather)
            return 'textureGather(%s, %s, %d)' % (s, coords, gather)
        if offset and gather is None:
            # GLSL wants constant offsets outside gathers: shift the coordinates by whole texels instead
            lv = 'int(%s)' % lod if lod is not None else '0'
            if kind == '2D':
                coords = '(%s + vec2(%s) / vec2(textureSize(%s, %s)))' % (coords, offset, s, lv)
            elif kind == '2DARRAY':
                coords = 'vec3(%s.xy + vec2(%s) / vec2(textureSize(%s, %s).xy), %s.z)' % (coords, offset, s, lv, coords)
            elif kind == '3D':
                coords = '(%s + vec3(%s) / vec3(textureSize(%s, %s)))' % (coords, offset, s, lv)
            else:
                raise Unsupported('texel offset on %s' % kind)
            offset = None
        if dref is not None:
            # shadow: coordinates carry the reference; cube arrays take it separately
            if kind == 'CUBEARRAY':
                return 'texture(%s, %s, %s)' % (s, coords, dref)
            n = {'1D': 2, '2D': 3, 'CUBE': 4, '2DARRAY': 4, '1DARRAY': 3}[kind]
            c = 'vec%d(%s, %s)' % (n, coords, dref)
            if lod is not None:
                if kind in ('CUBE', '2DARRAY'):
                    return 'textureGrad(%s, %s, vec%d(0.0), vec%d(0.0))' % (s, c, 3 if kind == 'CUBE' else 2,
                                                                         3 if kind == 'CUBE' else 2)
                return 'textureLod(%s, %s, %s)' % (s, c, lod)
            return 'texture(%s, %s)' % (s, c)
        if lod is not None:
            if offset:
                return 'textureLodOffset(%s, %s, %s, %s)' % (s, coords, lod, offset)
            return 'textureLod(%s, %s, %s)' % (s, coords, lod)
        if bias is not None:
            if offset:
                return 'textureOffset(%s, %s, %s, %s)' % (s, coords, offset, bias)
            return 'texture(%s, %s, %s)' % (s, coords, bias)
        if offset:
            return 'textureOffset(%s, %s, %s)' % (s, coords, offset)
        return 'texture(%s, %s)' % (s, coords)

    def tex_coords(self, ttype, reg):
        """TEX/TLD4/TMML coordinate registers by TextureType"""
        f = lambda i: self.Rf(reg + i if reg != RZ else RZ)
        arr = lambda: 'float(%s & 0xffffu)' % self.R(reg)
        if ttype == 0:
            return '1D', f(0), reg + 1
        if ttype == 1:
            return '1DARRAY', 'vec2(%s, %s)' % (f(1), arr()), reg + 2
        if ttype == 2:
            return '2D', 'vec2(%s, %s)' % (f(0), f(1)), reg + 2
        if ttype == 3:
            return '2DARRAY', 'vec3(%s, %s, %s)' % (f(1), f(2), arr()), reg + 3
        if ttype == 4:
            return '3D', 'vec3(%s, %s, %s)' % (f(0), f(1), f(2)), reg + 3
        if ttype == 6:
            return 'CUBE', 'vec3(%s, %s, %s)' % (f(0), f(1), f(2)), reg + 3
        if ttype == 7:
            return 'CUBEARRAY', 'vec4(%s, %s, %s, %s)' % (f(1), f(2), f(3), arr()), reg + 4
        raise Unsupported('texture type %d' % ttype)

    def tex_offsets(self, kind, reg, bitsz=4, step=4):
        n = {'1D': 1, '1DARRAY': 1, '2D': 2, '2DARRAY': 2, '3D': 3, 'CUBE': 0, 'CUBEARRAY': 0}[kind]
        if not n:
            raise Unsupported('cube offsets')
        parts = ['bitfieldExtract(int(%s), %d, %d)' % (self.R(reg), i * step, bitsz) for i in range(n)]
        return parts[0] if n == 1 else 'ivec%d(%s)' % (n, ', '.join(parts))

    # ------------------------------------------------------------------ instructions
    def insn(self, pc, w):
        o = opcode(w)
        base, _, form = o.partition('_')
        if o in ('MOV32I',):
            base, form = 'MOV', '32I'
        if o.endswith('32I') and o not in ('MOV32I',):
            base, form = o[:-3], '32I'
        f = getattr(self, 'op_' + base, None)
        if f is None:
            raise Unsupported(o)
        f(w, form, pc)

    # --- floating point
    def op_FADD(self, w, form, pc):
        d, a = bits(w, 0, 8), bits(w, 8, 8)
        if form == '32I':
            sat, na, aa, nb, ab = False, bits(w, 56, 1), bits(w, 54, 1), bits(w, 53, 1), bits(w, 57, 1)
            if bits(w, 52, 1):
                raise Unsupported('FADD32I.CC')
        else:
            sat, na, aa, nb, ab = bits(w, 50, 1), bits(w, 48, 1), bits(w, 46, 1), bits(w, 45, 1), bits(w, 49, 1)
            if bits(w, 47, 1):
                raise Unsupported('FADD.CC')
        x = '%s + %s' % (self.fmod(self.Rf(a), na, aa), self.fmod(self.op_b_f(w, form), nb, ab))
        self.set_rf(d, self.sat('(%s)' % x, sat))

    def fmul_expr(self, a, b, fmz):
        if fmz == 2:
            self.uses.add('fmz')
            return 'fmz(%s, %s)' % (a, b)
        return '%s * %s' % (a, b)

    def op_FMUL(self, w, form, pc):
        d, a = bits(w, 0, 8), bits(w, 8, 8)
        if form == '32I':
            fmz, sat, nb, scale = bits(w, 53, 2), bits(w, 55, 1), 0, 0
            if bits(w, 52, 1):
                raise Unsupported('FMUL32I.CC')
        else:
            fmz, sat, nb, scale = bits(w, 44, 2), bits(w, 50, 1), bits(w, 48, 1), bits(w, 41, 3)
            if bits(w, 47, 1):
                raise Unsupported('FMUL.CC')
        x = self.fmul_expr(self.Rf(a), self.fmod(self.op_b_f(w, form), nb, 0), fmz)
        if scale:
            x = '(%s) * %s' % (x, {1: '0.5', 2: '0.25', 3: '0.125', 4: '8.0', 5: '4.0', 6: '2.0'}[scale])
        self.set_rf(d, self.sat('(%s)' % x, sat))

    def op_FFMA(self, w, form, pc):
        d, a = bits(w, 0, 8), bits(w, 8, 8)
        if form == '32I':
            b, c = self.op_b_f(w, '32I'), self.Rf(bits(w, 0, 8))
            na, nb, nc, sat, fmz = bits(w, 56, 1), 0, bits(w, 57, 1), bits(w, 55, 1), bits(w, 53, 2)
        else:
            if form == 'R':
                b, c = self.Rf(bits(w, 20, 8)), self.Rf(bits(w, 39, 8))
            elif form == 'RC':
                b, c = self.Rf(bits(w, 39, 8)), self.op_b_f(w, 'C')
            elif form == 'CR':
                b, c = self.op_b_f(w, 'C'), self.Rf(bits(w, 39, 8))
            else:
                b, c = self.op_b_f(w, 'I'), self.Rf(bits(w, 39, 8))
            na, nb, nc, sat, fmz = 0, bits(w, 48, 1), bits(w, 49, 1), bits(w, 50, 1), bits(w, 53, 2)
        if bits(w, 47, 1) and form != '32I' or form == '32I' and bits(w, 52, 1):
            raise Unsupported('FFMA.CC')
        av, bv, cv = self.fmod(self.Rf(a), na, 0), self.fmod(b, nb, 0), self.fmod(c, nc, 0)
        if fmz == 2:
            self.uses.add('fmz')
            x = 'ffmz(%s, %s, %s)' % (av, bv, cv)
        else:
            x = 'fma(%s, %s, %s)' % (av, bv, cv)
        self.set_rf(d, self.sat(x, sat))

    def op_MUFU(self, w, form, pc):
        d, a, op = bits(w, 0, 8), bits(w, 8, 8), bits(w, 20, 4)
        x = self.fmod(self.Rf(a), bits(w, 48, 1), bits(w, 46, 1))
        e = {0: 'cos(%s)', 1: 'sin(%s)', 2: 'exp2(%s)', 3: 'log2(%s)', 4: '(1.0 / %s)', 5: 'inversesqrt(%s)',
             8: 'sqrt(%s)'}.get(op)
        if e is None:
            raise Unsupported('MUFU %d' % op)
        self.set_rf(d, self.sat(e % x, bits(w, 50, 1)))

    def op_RRO(self, w, form, pc):
        # range reduction for MUFU.SIN/COS/EX2: MUFU takes plain values here, so RRO only applies modifiers
        d = bits(w, 0, 8)
        self.set_rf(d, self.fmod(self.op_b_f(w, form), bits(w, 45, 1), bits(w, 49, 1)))

    def op_FMNMX(self, w, form, pc):
        d, a = bits(w, 0, 8), bits(w, 8, 8)
        av = self.fmod(self.Rf(a), bits(w, 48, 1), bits(w, 46, 1))
        bv = self.fmod(self.op_b_f(w, form), bits(w, 45, 1), bits(w, 49, 1))
        p = self.P(bits(w, 39, 3), bits(w, 42, 1))
        if bits(w, 47, 1):
            raise Unsupported('FMNMX.CC')
        if p == 'true':
            self.set_rf(d, 'min(%s, %s)' % (av, bv))
        elif p == 'false':
            self.set_rf(d, 'max(%s, %s)' % (av, bv))
        else:
            self.set_rf(d, '(%s ? min(%s, %s) : max(%s, %s))' % (p, av, bv, av, bv))

    def op_FSETP(self, w, form, pc):
        av = self.fmod(self.Rf(bits(w, 8, 8)), bits(w, 43, 1), bits(w, 7, 1))
        bv = self.fmod(self.op_b_f(w, form), bits(w, 6, 1), bits(w, 44, 1))
        self.emit('{ float a_ = %s, b_ = %s;' % (av, bv))
        self.indent += 1
        cmp = self.fcmp(bits(w, 48, 4), 'a_', 'b_')
        bp = self.P(bits(w, 39, 3), bits(w, 42, 1))
        bo = bits(w, 45, 2)
        self.emit('bool c_ = %s;' % cmp)
        self.set_p(bits(w, 3, 3), self.bop(bo, 'c_', bp))
        self.set_p(bits(w, 0, 3), self.bop(bo, '!c_', bp))
        self.indent -= 1
        self.emit('}')

    def op_FSET(self, w, form, pc):
        av = self.fmod(self.Rf(bits(w, 8, 8)), bits(w, 43, 1), bits(w, 54, 1))
        bv = self.fmod(self.op_b_f(w, form), bits(w, 53, 1), bits(w, 44, 1))
        cmp = self.fcmp(bits(w, 48, 4), 'a_', 'b_')
        bp = self.P(bits(w, 39, 3), bits(w, 42, 1))
        t = '0x3f800000u' if bits(w, 52, 1) else '0xffffffffu'
        self.emit('{ float a_ = %s, b_ = %s; uint v_ = %s ? %s : 0u;' % (av, bv, self.bop(bits(w, 45, 2), cmp, bp), t))
        self.indent += 1
        self.set_r(bits(w, 0, 8), 'v_')
        if bits(w, 47, 1):
            self.uses.add('cc')
            self.emit('cc_z = v_ == 0u; cc_s = (v_ >> 31) != 0u; cc_c = false; cc_o = false;')
        self.indent -= 1
        self.emit('}')

    def op_F2F(self, w, form, pc):
        d = bits(w, 0, 8)
        dst, src = bits(w, 8, 2), bits(w, 10, 2)
        neg, ab, sat = bits(w, 45, 1), bits(w, 49, 1), bits(w, 50, 1)
        if src == 1:
            x = 'unpackHalf2x16(%s)[%d]' % (self.op_b_u(w, form), bits(w, 41, 1))
        elif src == 2:
            x = self.op_b_f(w, form)
        else:
            raise Unsupported('F2F from f64')
        x = self.fmod(x, neg, ab)
        if dst == 2 and src == 2:
            rop = bits(w, 39, 4)
            x = {8: 'roundEven(%s)', 9: 'floor(%s)', 10: 'ceil(%s)', 11: 'trunc(%s)'}.get(rop, '%s') % x
        x = self.sat(x, sat)
        if dst == 2:
            self.set_rf(d, x)
        elif dst == 1:
            self.set_r(d, 'packHalf2x16(vec2(%s, 0.0))' % x)
        else:
            raise Unsupported('F2F to f64')

    def op_F2I(self, w, form, pc):
        d = bits(w, 0, 8)
        dst, src, signed, rnd = bits(w, 8, 2), bits(w, 10, 2), bits(w, 12, 1), bits(w, 39, 2)
        if src == 1:
            x = 'unpackHalf2x16(%s)[%d]' % (self.op_b_u(w, form), bits(w, 41, 1))
        elif src == 2:
            x = self.op_b_f(w, form)
        else:
            raise Unsupported('F2I from f64')
        x = self.fmod(x, bits(w, 49, 1), bits(w, 45, 1))
        x = {0: 'roundEven(%s)', 1: 'floor(%s)', 2: 'ceil(%s)', 3: 'trunc(%s)'}[rnd] % x
        if dst == 3:
            raise Unsupported('F2I to 64-bit')
        if signed:
            lo, hi = (-32768.0, 32767.0) if dst == 1 else (-2147483648.0, 2147483520.0)
            e = 'uint(int(clamp(%s, %s, %s)))' % (x, lo, hi)
        else:
            hi = 65535.0 if dst == 1 else 4294967040.0
            e = 'uint(clamp(%s, 0.0, %s))' % (x, hi)
        self.emit('{ float x_ = %s; uint v_ = isnan(x_) ? 0u : %s;' % (x, e.replace(x, 'x_')))
        self.indent += 1
        if signed and dst == 2:
            # values above 2^31 - 128 clamp to INT_MAX
            self.emit('if (x_ >= 2147483648.0) v_ = 0x7fffffffu;')
        if not signed and dst == 2:
            self.emit('if (x_ >= 4294967296.0) v_ = 0xffffffffu;')
        self.set_r(d, 'v_')
        self.indent -= 1
        self.emit('}')

    def op_I2F(self, w, form, pc):
        d = bits(w, 0, 8)
        ff, ifmt, signed, sel = bits(w, 8, 2), bits(w, 10, 2), bits(w, 13, 1), bits(w, 41, 2)
        if ifmt == 3:
            raise Unsupported('I2F from 64-bit')
        src = self.op_b_u(w, form)
        width = {0: 8, 1: 16, 2: 32}[ifmt]
        if width < 32:
            v = 'bitfieldExtract(%s(%s), %d, %d)' % ('int' if signed else 'uint', src, sel * 8, width)
        else:
            v = 'int(%s)' % src if signed else src
        if bits(w, 49, 1):
            v = 'abs(%s)' % v if signed else v
        if bits(w, 45, 1):
            v = '(-%s)' % (v if signed else 'int(%s)' % v)
            signed = True
        x = 'float(%s)' % v
        if ff == 2:
            self.set_rf(d, x)
        elif ff == 1:
            self.set_r(d, 'packHalf2x16(vec2(%s, 0.0))' % x)
        else:
            raise Unsupported('I2F to f64')

    def op_I2I(self, w, form, pc):
        d = bits(w, 0, 8)
        dfmt, dsg, sfmt, ssg, sel = bits(w, 8, 2), bits(w, 12, 1), bits(w, 10, 2), bits(w, 13, 1), bits(w, 41, 3)
        neg, ab, sat = bits(w, 45, 1), bits(w, 49, 1), bits(w, 50, 1)
        src = self.op_b_u(w, form)
        sw = {0: 8, 1: 16, 2: 32}[sfmt]
        if sw < 32:
            v = 'bitfieldExtract(int(%s), %d, %d)' % (src, sel * 8, sw) if ssg else \
                'int(bitfieldExtract(%s, %d, %d))' % (src, sel * 8, sw)
        else:
            v = 'int(%s)' % src
        if ab:
            v = 'abs(%s)' % v
        if neg:
            v = '(-%s)' % v
        dw = {0: 8, 1: 16, 2: 32}[dfmt]
        if sat:
            if dsg:
                lo, hi = -(1 << (dw - 1)), (1 << (dw - 1)) - 1
            else:
                lo, hi = 0, (1 << dw) - 1 if dw < 32 else 0x7fffffff
            if not ssg and sw == 32 and not dsg:
                v = 'uint(%s)' % src
                self.set_r(d, v if dw == 32 else 'min(%s, %du)' % (v, (1 << dw) - 1))
                return
            v = 'clamp(%s, %d, %d)' % (v, lo, hi)
        if dw < 32:
            self.set_r(d, 'uint(bitfieldExtract(%s, 0, %d))' % (v, dw) if dsg else
                       'bitfieldExtract(uint(%s), 0, %d)' % (v, dw))
        else:
            self.set_r(d, 'uint(%s)' % v)

    # --- integer
    def cc_add(self, a, b, res, x=False):
        self.uses.add('cc')
        self.emit('cc_z = %s == 0u; cc_s = (%s >> 31) != 0u; cc_c = %s < %s%s; cc_o = ((~(%s ^ %s) & (%s ^ %s)) >> 31) != 0u;'
                  % (res, res, res, a, ' || (cc_c && %s == %s)' % (res, a) if x else '', a, b, a, res))

    def op_IADD(self, w, form, pc):
        d, a = bits(w, 0, 8), bits(w, 8, 8)
        if form == '32I':
            x, cc, sat, na, nb = bits(w, 53, 1), bits(w, 52, 1), bits(w, 54, 1), bits(w, 56, 1), 0
            po = bits(w, 55, 2) == 3
        else:
            x, cc, sat, na, nb = bits(w, 43, 1), bits(w, 47, 1), bits(w, 50, 1), bits(w, 49, 1), bits(w, 48, 1)
            po = bits(w, 48, 2) == 3
        if po:
            na = nb = 0
        if sat:
            raise Unsupported('IADD.SAT')
        av = self.R(a)
        bv = self.op_b_u(w, form)
        if na:
            av = '(-%s)' % av if av != '0u' else '0u'
        if nb:
            bv = '(-%s)' % bv
        self.emit('{ uint a_ = %s, b_ = %s; uint v_ = a_ + b_%s%s;' % (av, bv, ' + (cc_c ? 1u : 0u)' if x else '',
                                                                   ' + 1u' if po else ''))
        if x:
            self.uses.add('cc')
        self.indent += 1
        if cc:
            self.cc_add('a_', 'b_', 'v_', x)
        self.set_r(d, 'v_')
        self.indent -= 1
        self.emit('}')

    def op_IADD3(self, w, form, pc):
        d = bits(w, 0, 8)
        if bits(w, 47, 1) or bits(w, 48, 1):
            raise Unsupported('IADD3.CC/X')
        a, c = self.R(bits(w, 8, 8)), self.R(bits(w, 39, 8))
        b = self.op_b_u(w, form)
        if form == 'R':
            def half(v, h):
                return {0: v, 1: '(%s & 0xffffu)' % v, 2: '(%s >> 16)' % v}[h]
            a, b, c = half(a, bits(w, 35, 2)), half(b, bits(w, 33, 2)), half(c, bits(w, 31, 2))
        neg = lambda v, n: '(-%s)' % v if n else v
        a, b, c = neg(a, bits(w, 51, 1)), neg(b, bits(w, 50, 1)), neg(c, bits(w, 49, 1))
        if form == 'R' and bits(w, 37, 2):
            sh = bits(w, 37, 2)
            self.set_r(d, '((%s + %s) %s 16) + %s' % (a, b, '>>' if sh == 1 else '<<', c))
        else:
            self.set_r(d, '%s + %s + %s' % (a, b, c))

    def op_ISCADD(self, w, form, pc):
        d, a = bits(w, 0, 8), self.R(bits(w, 8, 8))
        if form == '32I':
            scale, na, nb = bits(w, 53, 5), 0, 0
        else:
            scale, na, nb = bits(w, 39, 5), bits(w, 49, 1), bits(w, 48, 1)
            if bits(w, 47, 1):
                raise Unsupported('ISCADD.CC')
        b = self.op_b_u(w, form)
        if na:
            a = '(-%s)' % a
        if nb:
            b = '(-%s)' % b
        self.set_r(d, '(%s << %d) + %s' % (a, scale, b))

    def op_XMAD(self, w, form, pc):
        d, a = bits(w, 0, 8), self.R(bits(w, 8, 8))
        asg, bsg, ha = bits(w, 48, 1), bits(w, 49, 1), bits(w, 53, 1)
        if bits(w, 47, 1):
            raise Unsupported('XMAD.CC')
        if form == 'R':
            b, c = self.R(bits(w, 20, 8)), self.R(bits(w, 39, 8))
            mode, hb, psl, mrg, x = bits(w, 50, 3), bits(w, 35, 1), bits(w, 36, 1), bits(w, 37, 1), bits(w, 38, 1)
        elif form == 'RC':
            b, c = self.R(bits(w, 39, 8)), self.op_b_u(w, 'C')
            mode, hb, psl, mrg, x = bits(w, 50, 2), bits(w, 52, 1), 0, 0, bits(w, 54, 1)
        elif form == 'CR':
            b, c = self.op_b_u(w, 'C'), self.R(bits(w, 39, 8))
            mode, hb, psl, mrg, x = bits(w, 50, 2), bits(w, 52, 1), bits(w, 55, 1), bits(w, 56, 1), bits(w, 54, 1)
        else:
            b, c = '%du' % bits(w, 20, 16), self.R(bits(w, 39, 8))
            mode, hb, psl, mrg, x = bits(w, 50, 3), 0, bits(w, 36, 1), bits(w, 37, 1), bits(w, 38, 1)
        if x:
            raise Unsupported('XMAD.X')

        def half(v, h, sg):
            if sg:
                return 'uint(bitfieldExtract(int(%s), %d, 16))' % (v, 16 * h)
            return '(%s >> 16)' % v if h else '(%s & 0xffffu)' % v
        self.emit('{ uint b_ = %s, c_ = %s;' % (b, c))
        self.indent += 1
        prod = '(%s * %s)' % (half(a, ha, asg), half('b_', hb, bsg))
        if psl:
            prod = '(%s << 16)' % prod
        cv = {0: 'c_', 1: '(c_ & 0xffffu)', 2: '(c_ >> 16)', 4: '((b_ << 16) + c_)'}.get(mode)
        if cv is None:
            raise Unsupported('XMAD mode %d' % mode)
        r = '%s + %s' % (prod, cv)
        if mrg:
            r = 'bitfieldInsert(%s, b_ & 0xffffu, 16, 16)' % r
        self.set_r(d, r)
        self.indent -= 1
        self.emit('}')

    def op_IMNMX(self, w, form, pc):
        raise Unsupported('IMNMX')

    def op_ISETP(self, w, form, pc):
        if bits(w, 43, 1):
            raise Unsupported('ISETP.X')
        a, b = self.R(bits(w, 8, 8)), self.op_b_u(w, form)
        cmp = self.icmp(bits(w, 49, 3), a, b, bits(w, 48, 1))
        bp = self.P(bits(w, 39, 3), bits(w, 42, 1))
        bo = bits(w, 45, 2)
        self.emit('{ bool c_ = %s;' % cmp)
        self.indent += 1
        self.set_p(bits(w, 3, 3), self.bop(bo, 'c_', bp))
        self.set_p(bits(w, 0, 3), self.bop(bo, '!c_', bp))
        self.indent -= 1
        self.emit('}')

    def op_ISET(self, w, form, pc):
        if bits(w, 43, 1) or bits(w, 47, 1):
            raise Unsupported('ISET.X/CC')
        a, b = self.R(bits(w, 8, 8)), self.op_b_u(w, form)
        cmp = self.icmp(bits(w, 49, 3), a, b, bits(w, 48, 1))
        bp = self.P(bits(w, 39, 3), bits(w, 42, 1))
        t = '0x3f800000u' if bits(w, 44, 1) else '0xffffffffu'
        self.set_r(bits(w, 0, 8), '(%s ? %s : 0u)' % (self.bop(bits(w, 45, 2), cmp, bp), t))

    def logic(self, a, b, op):
        if op == 3:
            return b
        return '(%s %s %s)' % (a, {0: '&', 1: '|', 2: '^'}[op], b)

    def pred_op(self, popd, dp, res):
        if dp == PT:
            return
        e = {0: 'false', 1: 'true', 2: '(%s == 0u)' % res, 3: '(%s != 0u)' % res}[popd]
        self.set_p(dp, e)

    def op_LOP(self, w, form, pc):
        d, a = bits(w, 0, 8), self.R(bits(w, 8, 8))
        b = self.op_b_u(w, form)
        if form == '32I':
            op, ia, ib, x, cc = bits(w, 53, 2), bits(w, 55, 1), bits(w, 56, 1), bits(w, 57, 1), bits(w, 52, 1)
            popd, dp = 0, PT
        else:
            op, ia, ib, x, cc = bits(w, 41, 2), bits(w, 39, 1), bits(w, 40, 1), bits(w, 43, 1), bits(w, 47, 1)
            popd, dp = bits(w, 44, 2), bits(w, 48, 3)
        if x or cc:
            raise Unsupported('LOP.X/CC')
        if ia:
            a = '(~%s)' % a
        if ib:
            b = '(~%s)' % b
        self.emit('{ uint v_ = %s;' % self.logic(a, b, op))
        self.indent += 1
        self.set_r(d, 'v_')
        self.pred_op(popd, dp, 'v_')
        self.indent -= 1
        self.emit('}')

    def op_LOP3(self, w, form, pc):
        d, a, c = bits(w, 0, 8), self.R(bits(w, 8, 8)), self.R(bits(w, 39, 8))
        b = self.op_b_u(w, form)
        if form == 'R':
            lut, popd, dp = bits(w, 28, 8), bits(w, 36, 2), bits(w, 48, 3)
            if bits(w, 38, 1) or bits(w, 47, 1):
                raise Unsupported('LOP3.X/CC')
        else:
            lut, popd, dp = bits(w, 48, 8), 0, PT
            if bits(w, 47, 1):
                raise Unsupported('LOP3.CC')
        terms = []
        for i in range(8):
            if lut >> i & 1:
                terms.append('(%s%s & %s%s & %s%s)' % ('' if i & 4 else '~', 'a_', '' if i & 2 else '~', 'b_',
                                                      '' if i & 1 else '~', 'c_'))
        e = ' | '.join(terms) if terms else '0u'
        self.emit('{ uint a_ = %s, b_ = %s, c_ = %s; uint v_ = %s;' % (a, b, c, e))
        self.indent += 1
        self.set_r(d, 'v_')
        self.pred_op(popd, dp, 'v_')
        self.indent -= 1
        self.emit('}')

    def op_SHL(self, w, form, pc):
        d, a = bits(w, 0, 8), self.R(bits(w, 8, 8))
        b = self.op_b_u(w, form)
        if bits(w, 43, 1) or bits(w, 47, 1):
            raise Unsupported('SHL.X/CC')
        if bits(w, 39, 1):
            self.set_r(d, '%s << (%s & 31u)' % (a, b))
        else:
            self.set_r(d, '(%s >= 32u ? 0u : %s << %s)' % (b, a, b))

    def op_SHR(self, w, form, pc):
        d, a = bits(w, 0, 8), self.R(bits(w, 8, 8))
        b = self.op_b_u(w, form)
        if bits(w, 43, 1) or bits(w, 47, 1) or bits(w, 40, 1):
            raise Unsupported('SHR.X/CC/BREV')
        wrap, sg = bits(w, 39, 1), bits(w, 48, 1)
        if wrap:
            sh = '(%s & 31u)' % b
            self.set_r(d, 'uint(int(%s) >> %s)' % (a, sh) if sg else '%s >> %s' % (a, sh))
        elif sg:
            self.set_r(d, 'uint(int(%s) >> min(%s, 31u))' % (a, b))
        else:
            self.set_r(d, '(%s >= 32u ? 0u : %s >> %s)' % (b, a, b))

    def op_BFE(self, w, form, pc):
        d, a = bits(w, 0, 8), self.R(bits(w, 8, 8))
        b = self.op_b_u(w, form)
        if bits(w, 40, 1) or bits(w, 47, 1):
            raise Unsupported('BFE.BREV/CC')
        sg = bits(w, 48, 1)
        self.emit('{ uint o_ = %s & 0xffu, n_ = (%s >> 8) & 0xffu; o_ = min(o_, 32u); n_ = min(n_, 32u - o_);' % (b, b))
        self.indent += 1
        if sg:
            self.set_r(d, 'n_ == 0u ? 0u : uint(bitfieldExtract(int(%s), int(o_), int(n_)))' % a)
        else:
            self.set_r(d, 'n_ == 0u ? 0u : bitfieldExtract(%s, int(o_), int(n_))' % a)
        self.indent -= 1
        self.emit('}')

    def op_MOV(self, w, form, pc):
        self.set_r(bits(w, 0, 8), self.op_b_u(w, form))

    def op_SEL(self, w, form, pc):
        d, a = bits(w, 0, 8), self.R(bits(w, 8, 8))
        b = self.op_b_u(w, form)
        p = self.P(bits(w, 39, 3), bits(w, 42, 1))
        self.set_r(d, '(%s ? %s : %s)' % (p, a, b))

    def op_PSETP(self, w, form, pc):
        pa = self.P(bits(w, 12, 3), bits(w, 15, 1))
        pb = self.P(bits(w, 29, 3), bits(w, 32, 1))
        pc_ = self.P(bits(w, 39, 3), bits(w, 42, 1))
        self.emit('{ bool c_ = %s;' % self.bop(bits(w, 24, 2), pa, pb))
        self.indent += 1
        self.set_p(bits(w, 3, 3), self.bop(bits(w, 45, 2), 'c_', pc_))
        self.set_p(bits(w, 0, 3), self.bop(bits(w, 45, 2), '!c_', pc_))
        self.indent -= 1
        self.emit('}')

    def op_PSET(self, w, form, pc):
        pa = self.P(bits(w, 12, 3), bits(w, 15, 1))
        pb = self.P(bits(w, 29, 3), bits(w, 32, 1))
        pc_ = self.P(bits(w, 39, 3), bits(w, 42, 1))
        if bits(w, 47, 1):
            raise Unsupported('PSET.CC')
        t = '0x3f800000u' if bits(w, 44, 1) else '0xffffffffu'
        r = self.bop(bits(w, 45, 2), self.bop(bits(w, 24, 2), pa, pb), pc_)
        self.set_r(bits(w, 0, 8), '(%s ? %s : 0u)' % (r, t))

    def op_CSETP(self, w, form, pc):
        t = self.flow(bits(w, 8, 5))
        bp = self.P(bits(w, 39, 3), bits(w, 42, 1))
        self.emit('{ bool c_ = %s;' % t)
        self.indent += 1
        self.set_p(bits(w, 3, 3), self.bop(bits(w, 45, 2), 'c_', bp))
        self.set_p(bits(w, 0, 3), self.bop(bits(w, 45, 2), '!c_', bp))
        self.indent -= 1
        self.emit('}')

    def op_NOP(self, w, form, pc):
        pass

    def op_DEPBAR(self, w, form, pc):
        pass

    def op_S2R(self, w, form, pc):
        sr = bits(w, 20, 8)
        e = {33: 'gl_LocalInvocationID.x', 34: 'gl_LocalInvocationID.y', 35: 'gl_LocalInvocationID.z',
             37: 'gl_WorkGroupID.x', 38: 'gl_WorkGroupID.y', 39: 'gl_WorkGroupID.z', 0: '0u',
             32: '(gl_LocalInvocationID.x | gl_LocalInvocationID.y << 16 | gl_LocalInvocationID.z << 26)'}.get(sr)
        if e is None:
            raise Unsupported('S2R %d' % sr)
        self.set_r(bits(w, 0, 8), e)

    # --- memory
    def op_LDC(self, w, form, pc):
        d, a, off, idx = bits(w, 0, 8), bits(w, 8, 8), sext(bits(w, 20, 16), 16), bits(w, 36, 5)
        mode, size = bits(w, 44, 2), bits(w, 48, 3)
        if mode != 0:
            raise Unsupported('LDC mode %d' % mode)
        if a == RZ:
            if size == 4:
                self.set_r(d, self.cbuf(idx, off))
            elif size == 5:
                self.set_r(d, self.cbuf(idx, off))
                self.set_r(d + 1, self.cbuf(idx, off + 4))
            else:
                raise Unsupported('LDC size %d' % size)
            return
        addr = '(%s + %du)' % (self.R(a), off & 0xffffffff) if off else self.R(a)
        self.emit('{ uint ad_ = %s;' % addr)
        self.indent += 1
        if size == 4:
            self.set_r(d, self.cbuf_dyn(idx, 'ad_'))
        elif size == 5:
            v0, v1 = self.cbuf_dyn(idx, 'ad_'), self.cbuf_dyn(idx, 'ad_ + 4u')
            self.emit('uint v0_ = %s, v1_ = %s;' % (v0, v1))
            self.set_r(d, 'v0_')
            self.set_r(d + 1, 'v1_')
        else:
            raise Unsupported('LDC size %d' % size)
        self.indent -= 1
        self.emit('}')

    def global_addr(self, w):
        a, off, e = bits(w, 8, 8), sext(bits(w, 20, 24), 24), bits(w, 45, 1)
        self.uses.add('global')
        if e:
            base = 'packUint2x32(uvec2(%s, %s))' % (self.R(a), self.R(a + 1 if a != RZ else RZ))
        else:
            base = 'uint64_t(%s)' % self.R(a)
        return '(%s + %d)' % (base, off) if off else base

    def op_LDG(self, w, form, pc):
        d, size = bits(w, 0, 8), bits(w, 48, 3)
        n = {4: 1, 5: 2, 6: 4}.get(size)
        if n is None:
            raise Unsupported('LDG size %d' % size)
        self.emit('{ uint64_t ga_ = %s;' % self.global_addr(w))
        self.indent += 1
        for i in range(n):
            self.set_r(d + i, 'GMem(ga_ + %d).v' % (4 * i))
        self.indent -= 1
        self.emit('}')

    def op_STG(self, w, form, pc):
        d, size = bits(w, 0, 8), bits(w, 48, 3)
        n = {4: 1, 5: 2, 6: 4}.get(size)
        if n is None:
            raise Unsupported('STG size %d' % size)
        self.emit('{ uint64_t ga_ = %s;' % self.global_addr(w))
        self.indent += 1
        for i in range(n):
            self.emit('GMem(ga_ + %d).v = %s;' % (4 * i, self.R(d + i if d != RZ else RZ)))
        self.indent -= 1
        self.emit('}')

    def local_addr(self, w):
        a, off = bits(w, 8, 8), sext(bits(w, 20, 24), 24)
        self.uses.add('lmem')
        if a == RZ:
            return '%du' % (off & 0xffffffff)
        return '(%s + %du)' % (self.R(a), off & 0xffffffff)

    def op_LDL(self, w, form, pc):
        d, size = bits(w, 0, 8), bits(w, 48, 3)
        n = {4: 1, 5: 2, 6: 4}.get(size)
        if n is None:
            raise Unsupported('LDL size %d' % size)
        self.emit('{ uint la_ = %s >> 2;' % self.local_addr(w))
        self.indent += 1
        for i in range(n):
            self.set_r(d + i, 'lmem[(la_ + %du) & LMEM_MASK]' % i)
        self.indent -= 1
        self.emit('}')

    def op_STL(self, w, form, pc):
        d, size = bits(w, 0, 8), bits(w, 48, 3)
        n = {4: 1, 5: 2, 6: 4}.get(size)
        if n is None:
            raise Unsupported('STL size %d' % size)
        self.emit('{ uint la_ = %s >> 2;' % self.local_addr(w))
        self.indent += 1
        for i in range(n):
            self.emit('lmem[(la_ + %du) & LMEM_MASK] = %s;' % (i, self.R(d + i if d != RZ else RZ)))
        self.indent -= 1
        self.emit('}')

    # --- attributes
    def op_ALD(self, w, form, pc):
        d, idx, off, size = bits(w, 0, 8), bits(w, 8, 8), bits(w, 20, 10), bits(w, 47, 2)
        if idx != RZ or bits(w, 32, 1) or bits(w, 31, 1):
            raise Unsupported('ALD indexed/output/patch')
        for i in range(size + 1):
            e, _ = self.attr_in(off + 4 * i)
            self.set_r(d + i, e)

    def op_AST(self, w, form, pc):
        s, idx, off, size = bits(w, 0, 8), bits(w, 8, 8), bits(w, 20, 10), bits(w, 47, 2)
        if idx != RZ or bits(w, 31, 1):
            raise Unsupported('AST indexed/patch')
        for i in range(size + 1):
            self.attr_out(off + 4 * i, self.R(s + i if s != RZ else RZ))

    def op_IPA(self, w, form, pc):
        d, attr, mode, sat = bits(w, 0, 8), bits(w, 30, 8) * 4, bits(w, 54, 2), bits(w, 51, 1)
        if bits(w, 38, 1):
            raise Unsupported('IPA indexed')
        e, generic = self.attr_in(attr)
        # GLSL inputs are already perspective-correct: IPA.MUL's multiplier (1/w from IPA a[0x7c]) is not needed
        if sat:
            e = 'U(clamp(F(%s), 0.0, 1.0))' % e
        self.set_r(d, e)

    # --- textures
    def store_mask(self, dest, mask, val, shadow=False):
        r = dest
        for c in range(4):
            if mask >> c & 1:
                if shadow:
                    self.set_rf(r, 'v_' if c < 3 else '1.0')
                else:
                    self.set_rf(r, 'v_[%d]' % c)
                r += 1

    def op_TEXS(self, w, form, pc):
        da, a, b, db = bits(w, 0, 8), bits(w, 8, 8), bits(w, 20, 8), bits(w, 28, 8)
        h, sw, enc, f32 = self.tex_handle(bits(w, 36, 13)), bits(w, 50, 3), bits(w, 53, 4), bits(w, 59, 1)
        fa = lambda i: self.Rf(a + i if a != RZ else RZ)
        fb = lambda i: self.Rf(b + i if b != RZ else RZ)
        arr = lambda: 'float(%s & 0xffffu)' % self.R(a)
        dref = None
        if enc == 0:
            e = self.sample('1D', 'h_', fa(0), lod='0.0')
        elif enc == 1:
            e = self.sample('2D', 'h_', 'vec2(%s, %s)' % (fa(0), fb(0)))
        elif enc == 2:
            e = self.sample('2D', 'h_', 'vec2(%s, %s)' % (fa(0), fb(0)), lod='0.0')
        elif enc == 3:
            e = self.sample('2D', 'h_', 'vec2(%s, %s)' % (fa(0), fa(1)), lod=fb(0))
        elif enc == 4:
            dref = True
            e = self.sample('2D', 'h_', 'vec2(%s, %s)' % (fa(0), fa(1)), dref=fb(0))
        elif enc == 5:
            dref = True
            e = self.sample('2D', 'h_', 'vec2(%s, %s)' % (fa(0), fa(1)), dref=fb(1), lod=fb(0))
        elif enc == 6:
            dref = True
            e = self.sample('2D', 'h_', 'vec2(%s, %s)' % (fa(0), fa(1)), dref=fb(0), lod='0.0')
        elif enc == 7:
            e = self.sample('2DARRAY', 'h_', 'vec3(%s, %s, %s)' % (fa(1), fb(0), arr()))
        elif enc == 8:
            e = self.sample('2DARRAY', 'h_', 'vec3(%s, %s, %s)' % (fa(1), fb(0), arr()), lod='0.0')
        elif enc == 9:
            dref = True
            e = self.sample('2DARRAY', 'h_', 'vec3(%s, %s, %s)' % (fa(1), fb(0), arr()), dref=fb(1), lod='0.0')
        elif enc == 10:
            e = self.sample('3D', 'h_', 'vec3(%s, %s, %s)' % (fa(0), fa(1), fb(0)))
        elif enc == 11:
            e = self.sample('3D', 'h_', 'vec3(%s, %s, %s)' % (fa(0), fa(1), fb(0)), lod='0.0')
        elif enc == 12:
            e = self.sample('CUBE', 'h_', 'vec3(%s, %s, %s)' % (fa(0), fa(1), fb(0)))
        elif enc == 13:
            e = self.sample('CUBE', 'h_', 'vec3(%s, %s, %s)' % (fa(0), fa(1), fb(0)), lod=fb(1))
        else:
            raise Unsupported('TEXS encoding %d' % enc)
        masks = [1, 2, 4, 8, 3, 9, 10, 12] if db == RZ else [7, 11, 13, 14, 15]
        if sw >= len(masks):
            raise Unsupported('TEXS swizzle')
        mask = masks[sw]
        self.emit('{ uint h_ = %s; %s v_ = %s;' % (h, 'float' if dref else 'vec4', e))
        self.indent += 1
        comps = [c for c in range(4) if mask >> c & 1]
        val = lambda c: ('v_' if c < 3 else '1.0') if dref else 'v_[%d]' % c
        if f32:
            for i, c in enumerate(comps):
                if i < 2 and da != RZ:
                    self.set_rf(da + i, val(c))
                elif i >= 2 and db != RZ:
                    self.set_rf(db + i - 2, val(c))
        else:
            vals = [val(c) for c in comps]
            pack = lambda x, y: 'packHalf2x16(vec2(%s, %s))' % (x, y)
            if len(vals) >= 1:
                self.set_r(da, pack(vals[0], vals[1] if len(vals) > 1 else '0.0'))
            if len(vals) >= 3:
                self.set_r(db, pack(vals[2], vals[3] if len(vals) > 3 else '0.0'))
        self.indent -= 1
        self.emit('}')

    def op_TEX(self, w, form, pc):
        if form == 'B':
            raise Unsupported('TEX.B')
        d, cr, mr = bits(w, 0, 8), bits(w, 8, 8), bits(w, 20, 8)
        ttype, mask, dc = bits(w, 28, 3), bits(w, 31, 4), bits(w, 50, 1)
        aoffi, blod, lc = bits(w, 54, 1), bits(w, 55, 3), bits(w, 58, 1)
        if lc or bits(w, 51, 3) != PT:
            raise Unsupported('TEX.LC/sparse')
        kind, coords, _ = self.tex_coords(ttype, cr)
        meta = mr
        lod = bias = None
        if blod in (2, 3, 6, 7):
            v = self.Rf(meta)
            meta += 1
            if blod in (2, 6):
                bias = v
            else:
                lod = v
        elif blod == 1:
            lod = '0.0'
        off = None
        if aoffi:
            off = self.tex_offsets(kind, meta)
            meta += 1
        dref = None
        if dc:
            dref = self.Rf(meta)
            meta += 1
        e = self.sample(kind, 'h_', coords, lod=lod, bias=bias, dref=dref, offset=off)
        self.emit('{ uint h_ = %s; %s v_ = %s;' % (self.tex_handle(bits(w, 36, 13)), 'float' if dc else 'vec4', e))
        self.indent += 1
        self.store_mask(d, mask, 'v_', shadow=bool(dc))
        self.indent -= 1
        self.emit('}')

    def op_TLD4(self, w, form, pc):
        if form == 'B':
            raise Unsupported('TLD4.B')
        d, cr, mr = bits(w, 0, 8), bits(w, 8, 8), bits(w, 20, 8)
        ttype, mask, dc, comp, ot = bits(w, 28, 3), bits(w, 31, 4), bits(w, 50, 1), bits(w, 56, 2), bits(w, 54, 2)
        if bits(w, 51, 3) != PT or ot > 1:
            raise Unsupported('TLD4 sparse/PTP')
        kind, coords, _ = self.tex_coords(ttype, cr)
        meta = mr
        off = None
        if ot == 1:
            off = self.tex_offsets(kind, meta, 6, 8)
            meta += 1
        dref = None
        if dc:
            dref = self.Rf(meta)
            meta += 1
        e = self.sample(kind, 'h_', coords, dref=dref, offset=off, gather=comp)
        self.emit('{ uint h_ = %s; vec4 v_ = %s;' % (self.tex_handle(bits(w, 36, 13)), e))
        self.indent += 1
        self.store_mask(d, mask, 'v_')
        self.indent -= 1
        self.emit('}')

    def op_TLD4S(self, w, form, pc):
        da, a, b, db = bits(w, 0, 8), bits(w, 8, 8), bits(w, 20, 8), bits(w, 28, 8)
        dc, aoffi, comp, f16 = bits(w, 50, 1), bits(w, 51, 1), bits(w, 52, 2), bits(w, 55, 1)
        # coordinates in Ra, Ra+1; offsets / reference in Rb (as TEXS lays them out)
        coords = 'vec2(%s, %s)' % (self.Rf(a), self.Rf(a + 1 if a != RZ else RZ))
        off = dref = None
        if aoffi:
            off = 'ivec2(bitfieldExtract(int(%s), 0, 6), bitfieldExtract(int(%s), 8, 6))' % (self.R(b), self.R(b))
            if dc:
                dref = self.Rf(b + 1 if b != RZ else RZ)
        elif dc:
            dref = self.Rf(b)
        e = self.sample('2D', 'h_', coords, dref=dref, offset=off, gather=comp)
        self.emit('{ uint h_ = %s; vec4 v_ = %s;' % (self.tex_handle(bits(w, 36, 13)), e))
        self.indent += 1
        if f16:
            self.set_r(da, 'packHalf2x16(v_.xy)')
            self.set_r(db, 'packHalf2x16(v_.zw)')
        else:
            for i, r in enumerate([da, da + 1, db, db + 1]):
                if (i < 2 and da != RZ) or (i >= 2 and db != RZ):
                    self.set_rf(r, 'v_[%d]' % i)
        self.indent -= 1
        self.emit('}')

    def op_TMML(self, w, form, pc):
        d, cr, mr = bits(w, 0, 8), bits(w, 8, 8), bits(w, 20, 8)
        ttype, mask = bits(w, 28, 3), bits(w, 31, 4)
        kind, coords, _ = self.tex_coords(ttype, cr)
        f = lambda i: self.Rf(cr + i if cr != RZ else RZ)
        if kind == '2DARRAY':
            coords = 'vec2(%s, %s)' % (f(1), f(2))
        elif kind == 'CUBEARRAY':
            coords = 'vec3(%s, %s, %s)' % (f(1), f(2), f(3))
        elif kind == '1DARRAY':
            coords = f(1)
        if form == 'B':
            h = self.R(mr)
        else:
            h = self.tex_handle(bits(w, 36, 13))
        e = self.sample(kind, 'h_', coords, query=True)
        self.emit('{ uint h_ = %s; vec2 v_ = %s;' % (h, e))
        self.indent += 1
        r = d
        for c in range(4):
            if mask >> c & 1:
                if c < 2:
                    self.set_r(r, 'uint(v_[%d] * 256.0)' % c)
                else:
                    self.set_r(r, '0u')
                r += 1
        self.indent -= 1
        self.emit('}')

    def op_TXQ(self, w, form, pc):
        raise Unsupported('TXQ')

    def op_KIL(self, w, form, pc):
        t = self.flow(bits(w, 0, 5))
        if t == 'true':
            self.emit('discard;')
        elif t != 'false':
            self.emit('if (%s) discard;' % t)

    # ------------------------------------------------------------------ outputs
    def exit_outputs(self):
        if self.stage != 'fragment':
            return
        if self.omap_target & 0xf:
            # NVN's alpha test (nvnColorStateSetAlphaTest + SetAlphaRef) on target 0, from push constants
            self.emit('if (!alpha_ok(%s)) discard;' % (self.Rf(3) if self.omap_target & 8 else '1.0'))
        r = 0
        for rt in range(8):
            m = self.omap_target >> (4 * rt) & 0xf
            if not m:
                continue
            for c in range(4):
                if m >> c & 1:
                    self.emit('out_c%d[%d] = %s;' % (rt, c, self.Rf(r)))
                r += 1
        if self.omap_flags & 1:
            self.emit('gl_SampleMask[0] = int(%s);' % self.R(r))
        if self.omap_flags & 2:
            self.uses.add('fragdepth')
            self.emit('gl_FragDepth = %s;' % self.Rf(r + 1))

    # ------------------------------------------------------------------ control flow
    def translate(self):
        ins = list(self.sh.instructions())
        n = len(ins)
        idx_of = {pc: i for i, (pc, w) in enumerate(ins)}
        # leaders and the end (the trailing `BRA self` / zero padding is cut off)
        end = n
        for i, (pc, w) in enumerate(ins):
            o = opcode(w)
            if o == 'BRA':
                t = pc + sext(bits(w, 20, 24), 24) + 8
                if t % 32 == 0:
                    t += 8
                if t >= pc - 8 and t <= pc and bits(w, 16, 4) == PT:     # `BRA self`: the end
                    end = min(end, i)
        leaders = {0}
        targets = {}
        for i in range(end):
            pc, w = ins[i]
            o = opcode(w)
            if o in ('BRA', 'SSY'):
                t = pc + sext(bits(w, 20, 24), 24) + 8
                if t % 32 == 0:
                    t += 8                  # a scheduling word: the group's first instruction
                if t not in idx_of:
                    raise Unsupported('branch target %#x outside the code' % t)
                ti = idx_of[t]
                if ti <= i and o == 'BRA':
                    raise Unsupported('backward branch (loop)')
                targets[i] = ti
                if o == 'BRA':
                    leaders.add(ti)
                    leaders.add(i + 1)
                else:
                    leaders.add(ti)
            elif o in ('EXIT', 'SYNC'):
                leaders.add(i + 1)
            elif o in ('BRK', 'CONT', 'PBK', 'PCNT', 'CAL', 'RET', 'JMP', 'BRX', 'JMX'):
                raise Unsupported(o)
        leaders = sorted(l for l in leaders if l < end)
        blocks = [(leaders[k], leaders[k + 1] if k + 1 < len(leaders) else end) for k in range(len(leaders))]
        bid = {s: k for k, (s, e) in enumerate(blocks)}
        # reconvergence stacks (SSY targets) at each block's entry
        stacks = {0: ()}
        reach_needed = set()
        body = []
        self.lines = body
        for k, (s, e) in enumerate(blocks):
            if k not in stacks:
                continue            # unreachable
            stack = list(stacks[k])
            if k:
                self.emit('if (b%d) {' % k)
                reach_needed.add(k)
                self.indent += 1
            term = None
            for i in range(s, e):
                pc, w = ins[i]
                o = opcode(w)
                pi, pn = bits(w, 16, 3), bits(w, 19, 1)
                cond = self.P(pi, pn)
                if cond == 'false':
                    continue
                if o == 'SSY':
                    stack.append(targets[i])
                    continue
                if o in ('BRA', 'SYNC', 'EXIT'):
                    if o == 'BRA':
                        t = self.flow(bits(w, 0, 5))
                        if t != 'true':
                            cond = t if cond == 'true' else '(%s && %s)' % (cond, t)
                    elif o == 'EXIT' and bits(w, 0, 5) != 15:
                        t = self.flow(bits(w, 0, 5))
                        cond = t if cond == 'true' else '(%s && %s)' % (cond, t)
                    term = (o, cond, i)
                    break
                if cond != 'true':
                    self.emit('if (%s) {' % cond)
                    self.indent += 1
                self.insn(pc, w)
                if cond != 'true':
                    self.indent -= 1
                    self.emit('}')
            # successors
            succ = []
            nxt = k + 1 if e < end else None
            if term is None:
                if nxt is not None:
                    succ.append((nxt, 'true', tuple(stack)))
            else:
                o, cond, i = term
                if o == 'BRA':
                    succ.append((bid[targets[i]], cond, tuple(stack)))
                    if cond != 'true' and nxt is not None:
                        succ.append((nxt, '!(%s)' % cond, tuple(stack)))
                elif o == 'SYNC':
                    if not stack:
                        raise Unsupported('SYNC without SSY')
                    t = stack[-1]
                    if t not in bid:
                        raise Unsupported('SYNC target is not a block')
                    succ.append((bid[t], cond, tuple(stack[:-1])))
                    if cond != 'true' and nxt is not None:
                        succ.append((nxt, '!(%s)' % cond, tuple(stack)))
                else:   # EXIT
                    if cond != 'true':
                        self.emit('if (%s) {' % cond)
                        self.indent += 1
                    self.exit_outputs()
                    self.emit('return;')
                    if cond != 'true':
                        self.indent -= 1
                        self.emit('}')
                        if nxt is not None:
                            succ.append((nxt, 'true', tuple(stack)))
            for t, c, st in succ:
                if t <= k:
                    raise Unsupported('backward edge')
                if t in stacks and stacks[t] != st:
                    # SSY targets popped on one path only: keep the shorter stack (they reconverge there)
                    if len(st) < len(stacks[t]):
                        stacks[t] = st
                else:
                    stacks.setdefault(t, st)
                self.emit(('b%d = true;' % t) if c == 'true' else 'if (%s) b%d = true;' % (c, t))
            if k:
                self.indent -= 1
                self.emit('}')
        self.exit_outputs()
        return body, sorted(reach_needed)

    # ------------------------------------------------------------------ the whole shader
    def generate(self):
        body, flags = self.translate()
        out = ['#version 460',
               '#extension GL_EXT_nonuniform_qualifier : require',
               '#extension GL_EXT_samplerless_texture_functions : require']
        if 'global' in self.uses:
            out += ['#extension GL_EXT_buffer_reference : require',
                    '#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require']
        out.append('// translated from Maxwell shader %s (%s)' % (os.path.basename(self.sh.path), self.stage))
        if self.stage == 'compute':
            out += ['#ifndef LOCAL_X', '#define LOCAL_X 1', '#define LOCAL_Y 1', '#define LOCAL_Z 1', '#endif',
                    'layout(local_size_x = LOCAL_X, local_size_y = LOCAL_Y, local_size_z = LOCAL_Z) in;']
        out += ['float F(uint x) { return uintBitsToFloat(x); }', 'uint U(float x) { return floatBitsToUint(x); }']
        if 'fmz' in self.uses:
            out += ['float fmz(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }',
                    'float ffmz(float a, float b, float c) { return (a == 0.0 || b == 0.0) ? c : fma(a, b, c); }']
        base = STAGE_BASE[self.stage]
        for i in sorted(self.cbufs):
            out.append('layout(set = 0, binding = %d, std140) uniform CB%d { uvec4 d[4096]; } cb%d;' % (base + i, i, i))
            if 'cbdyn%d' % i in self.uses:
                out.append('uint cbr%d(uint a) { return a < 65536u ? cb%d.d[a >> 4][(a >> 2) & 3u] : 0u; }' % (i, i))
        if any(u.startswith('tex_') for u in self.uses):
            decl = {'t1d': 'texture1D', 't2d': 'texture2D', 't3d': 'texture3D', 'tcube': 'textureCube',
                    't2da': 'texture2DArray', 'tcubea': 'textureCubeArray', 't1da': 'texture1DArray'}
            for name, typ in decl.items():
                if 'tex_' + name in self.uses:
                    out.append('layout(set = 1, binding = 0) uniform %s %s[];' % (typ, name))
            out.append('layout(set = 2, binding = 0) uniform sampler smp[];')
        if 'global' in self.uses:
            out.append('layout(buffer_reference, std430, buffer_reference_align = 4) buffer GMem { uint v; };')
        if self.stage == 'vertex':
            for k in sorted(self.in_attrs):
                out.append('layout(location = %d) in vec4 in_a%d;' % (k, k))
            for k in sorted(self.out_attrs):
                out.append('layout(location = %d) out vec4 out_a%d;' % (k, k))
            if 'clip' in self.uses:
                out.append('out float gl_ClipDistance[8];')
        if self.stage == 'fragment':
            for k in sorted(self.in_attrs):
                q = self.in_attrs[k]
                out.append('layout(location = %d) %sin vec4 in_a%d;' % (k, '' if q == 'smooth' else q + ' ', k))
            for rt in range(8):
                if self.omap_target >> (4 * rt) & 0xf:
                    out.append('layout(location = %d) out vec4 out_c%d;' % (rt, rt))
        if self.stage == 'fragment':
            out += ['layout(push_constant) uniform PC { uint alpha_func; float alpha_ref; } pc;',
                    'bool alpha_ok(float a) {',
                    '    switch (pc.alpha_func) {',
                    '    case 1u: return false;',
                    '    case 2u: return a < pc.alpha_ref;',
                    '    case 3u: return a == pc.alpha_ref;',
                    '    case 4u: return a <= pc.alpha_ref;',
                    '    case 5u: return a > pc.alpha_ref;',
                    '    case 6u: return a != pc.alpha_ref;',
                    '    case 7u: return a >= pc.alpha_ref;',
                    '    default: return true;',
                    '    }',
                    '}']
        if 'lmem' in self.uses:
            words = max(16, (self.sh.local_memory_size() + 3) // 4)
            p2 = 1
            while p2 < words:
                p2 <<= 1
            out.append('#define LMEM_MASK %du' % (p2 - 1))
            out.append('uint lmem[%d];' % p2)
        out.append('void main() {')
        regs = sorted(self.regs)
        for i in range(0, len(regs), 16):
            out.append('    uint ' + ', '.join('r%d = 0u' % r for r in regs[i:i + 16]) + ';')
        out.append('    bool p0 = false, p1 = false, p2 = false, p3 = false, p4 = false, p5 = false, p6 = false;')
        if 'cc' in self.uses:
            out.append('    bool cc_z = false, cc_s = false, cc_c = false, cc_o = false;')
        for i in range(0, len(flags), 16):
            out.append('    bool ' + ', '.join('b%d = false' % f for f in flags[i:i + 16]) + ';')
        if self.stage == 'vertex':
            out.append('    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);')
        out += body
        out.append('}')
        return '\n'.join(out) + '\n'


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    check = '--check' in sys.argv
    spv = '--spv' in sys.argv
    here = os.path.dirname(os.path.abspath(__file__))
    src = args[0] if args else os.path.join(here, '../../../build/shaders')
    dst = args[1] if len(args) > 1 else os.path.join(src, 'glsl')
    os.makedirs(dst, exist_ok=True)
    ok, failed = 0, {}
    outputs = []
    for name in sorted(os.listdir(src)):
        if not name.endswith('.bin'):
            continue
        sh = Shader(os.path.join(src, name))
        h = name[:-4]
        try:
            text = Translator(sh).generate()
        except Unsupported as e:
            failed.setdefault(str(e), []).append(h)
            continue
        path = os.path.join(dst, '%s.%s' % (h, STAGE_EXT[sh.stage]))
        with open(path, 'w', newline='\n') as f:
            f.write(text)
        outputs.append(path)
        ok += 1
    print('%d translated, %d not' % (ok, sum(len(v) for v in failed.values())))
    for why, hs in sorted(failed.items(), key=lambda kv: -len(kv[1])):
        print('  %4d  %s  (e.g. %s)' % (len(hs), why, hs[0]))
    if check or spv:
        from concurrent.futures import ThreadPoolExecutor
        glslang = os.path.join(here, '../../../deps/glslang/bin/glslang.exe')
        spvdir = os.path.join(src, 'spv')
        os.makedirs(spvdir, exist_ok=True)

        def compile_one(p):
            h = os.path.basename(p).split('.')[0]
            outp = os.path.join(spvdir, h + '.spv') if spv else os.devnull
            r = subprocess.run([glslang, '-V', '--target-env', 'vulkan1.3', '-o', outp, p],
                               capture_output=True, text=True)
            return p, r.returncode, r.stdout
        bad = 0
        with ThreadPoolExecutor(os.cpu_count() or 4) as ex:
            for p, rc, out in ex.map(compile_one, outputs):
                if rc:
                    bad += 1
                    if bad <= 5:
                        print(p, out[-1500:])
        print('glslang: %d of %d failed' % (bad, len(outputs)))


if __name__ == '__main__':
    main()
