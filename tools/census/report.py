#!/usr/bin/env python3
"""Census report (PLAN.md P1 / M1): merge census runs + analyse the shader corpus → docs/census-report.md.

Usage: report.py <census_root> <corpus_dir> <out.md>
  census_root: $TF2_HOME/logs/census (one sub-directory per variant: census-<tag>.tsv, .threads.tsv, caps-<tag>.bin)
  corpus_dir:  content-addressed shader bytecode (<fnv64>.vs.bin / .ps.bin)
Enum names come from mingw-w64's d3d9types.h (public Direct3D 9 headers). The shader decoder follows the public
SM2/SM3 token format (instruction token: opcode bits 0-15, length bits 24-27; parameter tokens: register number
bits 0-10, type bits 28-30 | 11-12<<3).
"""
import collections, glob, os, re, struct, sys

# ---------------------------------------------------------------- enums from the headers
HDR = glob.glob('/opt/homebrew/Cellar/mingw-w64/*/toolchain-x86_64/x86_64-w64-mingw32/include/d3d9types.h')[0]
SRC = open(HDR).read()

def fourcc(s):
    return s[0] | s[1] << 8 | s[2] << 16 | s[3] << 24

def enum(prefix):
    out = {}
    for name, val in re.findall(r'\b(' + prefix + r'[A-Z0-9_]+)\s*=\s*([^,\n/]+)', SRC):
        v = val.strip()
        m = re.match(r"MAKEFOURCC\('(.)',\s*'(.)',\s*'(.)',\s*'(.)'\)", v)
        try:
            n = fourcc([ord(c) for c in m.groups()]) if m else int(eval(v.replace('D3DSP_TEXTURETYPE_SHIFT', '27'), {}))
        except Exception:
            continue
        out.setdefault(n, name[len(prefix):])
    return out

RS, SAMP, TSS, FMT = enum('D3DRS_'), enum('D3DSAMP_'), enum('D3DTSS_'), enum('D3DFMT_')
DECLTYPE, DECLUSAGE, PT, QT = enum('D3DDECLTYPE_'), enum('D3DDECLUSAGE_'), enum('D3DPT_'), enum('D3DQUERYTYPE_')
SIO, SPR, STT = enum('D3DSIO_'), enum('D3DSPR_'), enum('D3DSTT_')
RTYPE = {1: 'SURFACE', 2: 'VOLUME', 3: 'TEXTURE', 4: 'VOLUMETEXTURE', 5: 'CUBETEXTURE', 6: 'VERTEXBUFFER', 7: 'INDEXBUFFER'}
SPR.update({3: 'ADDR/TEXTURE', 6: 'TEXCRDOUT/OUTPUT'})

def fmt_name(s):
    if s.isdigit():
        return FMT.get(int(s), s)
    return s  # FOURCC text already

def usage_s(u):
    names = {0x1: 'RT', 0x2: 'DEPTHSTENCIL', 0x8: 'WRITEONLY', 0x200: 'DYNAMIC', 0x400: 'AUTOGENMIPMAP',
             0x800: 'DMAP', 0x10000: 'QUERY_LEGACYBUMPMAP', 0x20000: 'QUERY_SRGBREAD', 0x40000: 'QUERY_FILTER',
             0x80000: 'QUERY_SRGBWRITE', 0x100000: 'QUERY_POSTPIXELSHADER_BLENDING', 0x200000: 'QUERY_VERTEXTEXTURE',
             0x400000: 'QUERY_WRAPANDMIP', 0x10: 'SOFTWAREPROCESSING', 0x20: 'DONOTCLIP', 0x40: 'POINTS', 0x80: 'RTPATCHES', 0x100: 'NPATCHES'}
    u = int(u, 16) if isinstance(u, str) else u
    parts = [n for b, n in names.items() if u & b]
    return '|'.join(parts) if parts else '0'

# ---------------------------------------------------------------- census input
def load(root):
    runs = {}
    for d in sorted(glob.glob(os.path.join(root, '*'))):
        tag = os.path.basename(d)
        f = os.path.join(d, f'census-{tag}.tsv')
        if not os.path.exists(f):
            continue
        c = collections.Counter()
        for line in open(f, errors='replace'):
            k, _, n = line.rstrip('\n').rpartition('\t')
            if k:
                c[k] += int(n)
        runs[tag] = c
    return runs

def cat(c, prefix):
    return {k.split('|', 1)[1]: n for k, n in c.items() if k.split('|', 1)[0] == prefix}

def table(rows, header):
    out = ['| ' + ' | '.join(header) + ' |', '|' + '---|' * len(header)]
    out += ['| ' + ' | '.join(str(x) for x in r) + ' |' for r in rows]
    return '\n'.join(out)

# ---------------------------------------------------------------- shader decoder (SM2/SM3)
def decode(words):
    """Yields (opcode, control, [param tokens]) for every instruction; skips comments."""
    i = 1
    n = len(words)
    while i < n:
        tok = words[i]
        op = tok & 0xffff
        if op == 0xffff:
            return
        if op == 0xfffe:
            i += 1 + ((tok >> 16) & 0x7fff)
            continue
        length = (tok >> 24) & 0xf
        yield op, (tok >> 16) & 0xff, tok, words[i + 1:i + 1 + length]
        i += 1 + length

def regtype(p):
    return ((p >> 28) & 7) | ((p >> 8) & 0x18)

def analyse_corpus(corpus):
    st = collections.defaultdict(collections.Counter)
    shaders = 0
    for path in glob.glob(os.path.join(corpus, '*.bin')):
        kind = 'vs' if path.endswith('.vs.bin') else 'ps'
        data = open(path, 'rb').read()
        words = struct.unpack(f'<{len(data) // 4}I', data)
        ver = f'{kind}_{(words[0] >> 8) & 0xff}_{words[0] & 0xff}'
        shaders += 1
        st['version'][ver] += 1
        seen_ops, seen_regs, max_c = set(), set(), -1
        ninstr = 0
        for op, ctrl, tok, params in decode(words):
            name = SIO.get(op, f'op{op}')
            ninstr += 1
            if op == 31:  # DCL: usage/sampler token, then dest
                u, d = params[0], params[1]
                rt = regtype(d)
                if rt == 10:  # sampler
                    st[f'{kind}.sampler_type'][STT.get(u & 0x78000000, hex(u))] += 1
                elif rt == 17:  # misc: vPos/vFace
                    st[f'{kind}.misc_input'][{0: 'vPos', 1: 'vFace'}.get(d & 0x7ff, '?')] += 1
                else:
                    usage = DECLUSAGE.get(u & 0x1f, u & 0x1f)
                    st[f'{kind}.dcl'][f'{SPR.get(rt, rt)} {usage}{(u >> 16) & 0xf}'] += 1
                    if d & (1 << 22):
                        st[f'{kind}.dcl_mod']['centroid'] += 1
                continue
            if op in (81, 47, 48):  # DEF, DEFI, DEFB
                continue
            if name == 'TEX' and ctrl:
                name = {1: 'TEXLDP', 2: 'TEXLDB'}.get(ctrl & 3, name)
            if name in ('IFC', 'BREAKC', 'SETP'):
                name += '.' + {1: 'gt', 2: 'eq', 3: 'ge', 4: 'lt', 5: 'ne', 6: 'le'}.get(ctrl & 7, '?')
            seen_ops.add(name)
            st[f'{kind}.op_total'][name] += 1
            if tok & (1 << 28):
                st[f'{kind}.features']['predicated'] += 1
            if tok & (1 << 30):
                st[f'{kind}.features']['coissue'] += 1
            for j, p in enumerate(params):
                rt = regtype(p)
                seen_regs.add(SPR.get(rt, rt))
                if rt == 2 or rt in (11, 12, 13):  # const float (incl. const2..4)
                    max_c = max(max_c, (p & 0x7ff) + {2: 0, 11: 2048, 12: 4096, 13: 6144}[rt])
                if p & (1 << 13):
                    st[f'{kind}.features']['relative_addressing'] += 1
                if j == 0 and op not in (2, 41, 42):  # dest of most ops
                    rm = (p >> 20) & 0xf
                    for b, n in ((1, 'saturate'), (2, 'partial_precision'), (4, 'centroid')):
                        if rm & b:
                            st[f'{kind}.dst_mod'][n] += 1
                    if (p >> 24) & 0xf:
                        st[f'{kind}.dst_mod']['shift'] += 1
                elif j > 0:
                    sm = (p >> 24) & 0xf
                    if sm:
                        st[f'{kind}.src_mod'][['none', 'neg', 'bias', 'biasneg', 'sign', 'signneg', 'comp', 'x2', 'x2neg',
                                               'dz', 'dw', 'abs', 'absneg', 'not'][sm] if sm < 14 else sm] += 1
        for o in seen_ops:
            st[f'{kind}.op_shaders'][o] += 1
        for r in seen_regs:
            st[f'{kind}.reg_shaders'][r] += 1
        st[f'{kind}.max_const'][max_c // 16 * 16 if max_c >= 0 else -1] += 1
        st[f'{kind}.instr'][min(ninstr // 32 * 32, 512)] += 1
    return shaders, st

# ---------------------------------------------------------------- caps
CAPS_FIELDS = ('DeviceType AdapterOrdinal Caps Caps2 Caps3 PresentationIntervals CursorCaps DevCaps PrimitiveMiscCaps '
               'RasterCaps ZCmpCaps SrcBlendCaps DestBlendCaps AlphaCmpCaps ShadeCaps TextureCaps TextureFilterCaps '
               'CubeTextureFilterCaps VolumeTextureFilterCaps TextureAddressCaps VolumeTextureAddressCaps LineCaps '
               'MaxTextureWidth MaxTextureHeight MaxVolumeExtent MaxTextureRepeat MaxTextureAspectRatio MaxAnisotropy '
               'f:MaxVertexW f:GuardBandLeft f:GuardBandTop f:GuardBandRight f:GuardBandBottom f:ExtentsAdjust StencilCaps '
               'FVFCaps TextureOpCaps MaxTextureBlendStages MaxSimultaneousTextures VertexProcessingCaps MaxActiveLights '
               'MaxUserClipPlanes MaxVertexBlendMatrices MaxVertexBlendMatrixIndex f:MaxPointSize MaxPrimitiveCount '
               'MaxVertexIndex MaxStreams MaxStreamStride VertexShaderVersion MaxVertexShaderConst PixelShaderVersion '
               'f:PixelShader1xMaxValue DevCaps2 f:MaxNpatchTessellationLevel Reserved5 MasterAdapterOrdinal '
               'AdapterOrdinalInGroup NumberOfAdaptersInGroup DeclTypes NumSimultaneousRTs StretchRectFilterCaps '
               'VS20Caps VS20DynamicFlowControlDepth VS20NumTemps VS20StaticFlowControlDepth PS20Caps '
               'PS20DynamicFlowControlDepth PS20NumTemps PS20StaticFlowControlDepth PS20NumInstructionSlots '
               'VertexTextureFilterCaps MaxVShaderInstructionsExecuted MaxPShaderInstructionsExecuted '
               'MaxVertexShader30InstructionSlots MaxPixelShader30InstructionSlots').split()

def caps(path):
    data = open(path, 'rb').read()
    vals = {}
    for i, f in enumerate(CAPS_FIELDS):
        raw = data[i * 4:i * 4 + 4]
        if len(raw) < 4:
            break
        if f.startswith('f:'):
            vals[f[2:]] = round(struct.unpack('<f', raw)[0], 3)
        else:
            vals[f] = struct.unpack('<I', raw)[0]
    return vals

# ---------------------------------------------------------------- report
def main():
    root, corpus, out = sys.argv[1:4]
    runs = load(root)
    allc = sum(runs.values(), collections.Counter())
    md = ['# D3D9 census — what TF2 actually uses', '',
          'Generated by `tools/census/report.py` from census runs (`tools/census/run-census.sh`). This is the authoritative scope '
          'for the tf2mt renderer (PLAN.md D9): anything not listed here is not implemented.', '',
          '## Runs', '', table([(t, sum(c.values())) for t, c in runs.items()], ['variant', 'recorded events']), '']

    def section(title, prefix, keyfmt=lambda k: k, top=None, note=''):
        rows = sorted(cat(allc, prefix).items(), key=lambda kv: -kv[1])
        if top:
            rows = rows[:top]
        per = {t: cat(c, prefix) for t, c in runs.items()}
        md.extend([f'## {title}', ''] + ([note, ''] if note else []))
        md.append(table([(keyfmt(k), n, ' '.join(t for t in runs if k in per[t])) for k, n in rows], ['value', 'count', 'variants']))
        md.append('')

    def fmtkey(k):
        return re.sub(r'fmt=([^ ]+)', lambda m: 'fmt=' + fmt_name(m.group(1)), re.sub(r'usage=(0x[0-9a-f]+)', lambda m: 'usage=' + usage_s(m.group(1)), k))

    section('Resources: 2D textures', 'create.tex2d', fmtkey)
    section('Resources: texture sizes', 'create.tex2d.size')
    section('Resources: cube textures', 'create.cube', fmtkey)
    section('Resources: volume textures', 'create.volume', fmtkey)
    section('Resources: render-target / depth / offscreen surfaces', 'create.rt_surface', fmtkey)
    section('Resources: depth-stencil surfaces', 'create.ds_surface', fmtkey)
    section('Resources: offscreen plain surfaces', 'create.offscreen', fmtkey)
    section('Resources: vertex buffers', 'create.vb', fmtkey)
    section('Resources: vertex buffer sizes', 'create.vb.size')
    section('Resources: index buffers', 'create.ib', fmtkey)
    section('Resources: index buffer sizes', 'create.ib.size')
    section('Locks: vertex buffers', 'lock.vb', fmtkey)
    section('Locks: vertex buffer lock sizes', 'lock.vb.size')
    section('Locks: index buffers', 'lock.ib', fmtkey)
    section('Locks: 2D textures', 'lock.tex2d', fmtkey)
    section('Locks: cube textures', 'lock.cube', fmtkey)
    section('Locks: volume textures', 'lock.volume', fmtkey)
    section('Locks: surfaces', 'lock.surface', fmtkey)
    section('CheckDeviceFormat (what Source asks, what it was told)', 'checkformat',
            lambda k: re.sub(r'rtype=(\d+)', lambda m: RTYPE.get(int(m.group(1)), m.group(1)), fmtkey(k)))
    section('CheckDeviceMultiSampleType', 'checkms', fmtkey)
    section('CheckDepthStencilMatch', 'checkdsmatch', lambda k: re.sub(r'(rt|ds)=([^ ]+)', lambda m: m.group(1) + '=' + fmt_name(m.group(2)), k))
    section('Queries created', 'create.query', lambda k: re.sub(r'type=(\d+)', lambda m: 'type=' + QT.get(int(m.group(1)), m.group(1)), k))
    section('Query usage', 'query.issue', lambda k: re.sub(r'type=(\d+)', lambda m: 'type=' + QT.get(int(m.group(1)), m.group(1)), k))
    section('Query polling', 'query.getdata', lambda k: re.sub(r'type=(\d+)', lambda m: 'type=' + QT.get(int(m.group(1)), m.group(1)), k))
    section('Draw calls', 'draw', lambda k: re.sub(r'prim=(\d+)', lambda m: 'prim=' + PT.get(int(m.group(1)), m.group(1)), k))
    section('Primitives per draw', 'draw.prims')
    section('Draws per frame', 'frame.draws')
    section('Vertex streams', 'stream')
    section('Instancing (SetStreamSourceFreq)', 'streamfreq')
    section('FVF', 'fvf')
    section('Vertex declaration elements', 'decl.element',
            lambda k: re.sub(r'usage=(\d+)', lambda m: 'usage=' + DECLUSAGE.get(int(m.group(1)), m.group(1)),
                             re.sub(r'type=(\d+)', lambda m: 'type=' + DECLTYPE.get(int(m.group(1)), m.group(1)), k)))
    section('Vertex declaration streams', 'decl.streams')
    section('Render targets bound', 'rt', fmtkey)
    section('Depth-stencil bound', 'ds', fmtkey)
    section('Clears', 'clear', lambda k: re.sub(r'flags=(0x[0-9a-f]+)', lambda m: 'flags=' + '|'.join(n for b, n in ((1, 'TARGET'), (2, 'ZBUFFER'), (4, 'STENCIL')) if int(m.group(1), 16) & b), k))
    section('StretchRect', 'stretchrect', fmtkey)
    section('Readback', 'readback', fmtkey)
    section('Uploads', 'upload', fmtkey)
    section('ColorFill', 'colorfill', fmtkey)
    section('Viewports', 'viewport')
    section('Scissor', 'scissor')
    section('User clip planes', 'clipplane')
    section('Shader constants (highest register written + 1, bucketed)', 'consts')
    section('Shader constants (exact maxima)', 'consts.max')
    section('Textures bound (stage, resource type)', 'settexture', lambda k: re.sub(r'type=(\d+)', lambda m: 'type=' + RTYPE.get(int(m.group(1)), m.group(1)), k))
    section('Samplers used', 'ss.sampler')
    section('State blocks', 'stateblock')
    section('GetDC / GenerateMipSubLevels', 'surface.GetDC')
    section('GenerateMipSubLevels', 'tex.GenerateMipSubLevels')

    # render / sampler / texture-stage states grouped by state
    for title, prefix, names in (('Render states', 'rs', RS), ('Sampler states', 'ss', SAMP)):
        groups = collections.defaultdict(list)
        for k, n in cat(allc, prefix).items():
            s, _, v = k.partition('=')
            groups[int(s)].append((v, n))
        md.extend([f'## {title} ({len(groups)} states used)', ''])
        md.append(table([(names.get(s, s), sum(n for _, n in vs), ' '.join(v for v, _ in sorted(vs, key=lambda x: -x[1])[:12]) + (' …' if len(vs) > 12 else ''))
                         for s, vs in sorted(groups.items())], ['state', 'sets', 'values (most frequent first)']))
        md.append('')
    tss = cat(allc, 'tss')
    md.extend(['## Texture stage states', '', table(sorted(tss.items(), key=lambda kv: -kv[1])[:40], ['stage state=value', 'count']) if tss else '(none — fixed-function texture stages unused)', ''])

    # threads
    md.extend(['## Threads', '', 'Per-thread device method counts (largest first) for the base run:', ''])
    base = next(iter(runs))
    tpath = os.path.join(root, base, f'census-{base}.threads.tsv')
    if os.path.exists(tpath):
        per = collections.defaultdict(collections.Counter)
        for line in open(tpath):
            tid, m, n = line.rstrip('\n').split('\t')
            per[tid][m] += int(n)
        for tid, c in sorted(per.items(), key=lambda kv: -sum(kv[1].values())):
            md.append(f'* tid {tid}: {sum(c.values())} calls — ' + ', '.join(f'{m} {n}' for m, n in c.most_common(8)))
        md.append('')

    # caps
    cp = os.path.join(root, base, f'caps-{base}.bin')
    if os.path.exists(cp):
        cv = caps(cp)
        keys = ['MaxTextureWidth', 'MaxTextureHeight', 'MaxVolumeExtent', 'MaxAnisotropy', 'MaxSimultaneousTextures',
                'MaxUserClipPlanes', 'MaxPrimitiveCount', 'MaxVertexIndex', 'MaxStreams', 'MaxStreamStride',
                'VertexShaderVersion', 'MaxVertexShaderConst', 'PixelShaderVersion', 'NumSimultaneousRTs',
                'PS20NumTemps', 'PS20NumInstructionSlots', 'MaxVertexShader30InstructionSlots', 'MaxPixelShader30InstructionSlots',
                'DeclTypes', 'StretchRectFilterCaps', 'VertexTextureFilterCaps', 'TextureCaps', 'RasterCaps', 'PrimitiveMiscCaps']
        md.extend(['## Caps reported by the oracle (what Source accepted)', '',
                   table([(k, hex(cv[k]) if 'Caps' in k or 'Version' in k or k == 'DeclTypes' else cv[k]) for k in keys if k in cv], ['field', 'value']), ''])

    # shader corpus
    n, st = analyse_corpus(corpus)
    md.extend([f'## Shader corpus ({n} unique shaders)', '', table(sorted(st['version'].items()), ['model', 'count']), ''])
    for kind in ('vs', 'ps'):
        tot = sum(v for k, v in st['version'].items() if k.startswith(kind))
        md.extend([f'### {kind}: opcodes ({len(st[kind + ".op_shaders"])} distinct)', '',
                   table([(o, st[kind + '.op_shaders'][o], st[kind + '.op_total'][o]) for o, _ in st[kind + '.op_shaders'].most_common()],
                         ['opcode', f'shaders using it (of {tot})', 'total instructions']), ''])
        for sub, title in (('reg_shaders', 'register types (shaders using)'), ('sampler_type', 'sampler types (dcl)'),
                           ('misc_input', 'vPos / vFace'), ('dcl', 'declared inputs/outputs'), ('src_mod', 'source modifiers'),
                           ('dst_mod', 'destination modifiers'), ('features', 'features'), ('max_const', 'highest float constant (bucket of 16)'),
                           ('instr', 'instruction count (bucket of 32)')):
            d = st[f'{kind}.{sub}']
            if d:
                md.extend([f'**{kind} {title}:** ' + ', '.join(f'{k} ×{v}' for k, v in sorted(d.items(), key=lambda kv: (-kv[1], str(kv[0])))), ''])
    open(out, 'w').write('\n'.join(md) + '\n')
    print(f'wrote {out}: {len(runs)} runs, {n} shaders')

if __name__ == '__main__':
    main()
