"""Averaging resolves of 2x and 4x render targets whose samples hold chosen
bytes: how the render backend averages 8_8_8_8_GAMMA (colour through the PWL
curve, alpha as 8-bit unorm), 8_8_8_8 and 2_10_10_10 samples, and converts the
average into k_8_8_8_8 ('8') and k_2_10_10_10 ('10').

Each sample of each pixel is written by its own draw (PA_SC_AA_MASK
0x1111 << k) of the rb suite's pixel shader into an 8_8_8_8 render target,
from texture 's k' whose texel is (byte + 1/4) / 255 per channel, so sample k
of pixel p holds the bytes BYTES[p][k] exactly. The Resolve then reads
the same EDRAM through RB_COLOR_INFO's format 8_8_8_8, 8_8_8_8_GAMMA or
2_10_10_10 ('8888', 'gamma', '1010102'): a converting Resolve averaging
samples 01 (2x), 0123, 01 or 23 (4x). 'raw' tests Resolve each sample's bits
(copy command 0), to check what the draws stored.

The pixels (p = x + 64 y), per channel independently:
- 0-255: all four samples equal;
- 256-767: one sample (p % 4) different from the other three, the two bytes
  from PWL segment edges (0-3, 31-32, 62-65, 94-97, 126-129, 190-193,
  253-255) or random;
- 768-1279: two and two, (a, b, a, b), (a, a, b, b) or (a, b, b, a) by
  p % 3, random bytes: (01)(23) pair-wise truncation and one sum differ here;
- 1280-2047: four random bytes.
"""

import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gpugen  # noqa: E402
import rb  # noqa: E402

WIDTH, HEIGHT = rb.WIDTH, rb.HEIGHT
# RB_COLOR_INFO color_format of each view.
VIEWS = {'8888': 0, 'gamma': 1, '1010102': 2}
# Resolve destination texture formats.
DESTS = {'8': 6, '10': 7}
# (samples, PA_SC_AA_CONFIG code via rb.registers, RB_COPY_CONTROL
# sample_select, the samples it averages, views, destinations)
AVERAGES = [
    (2, 1, 4, (0, 1), ('8888', 'gamma', '1010102'), ('8', '10')),
    (4, 2, 6, (0, 1, 2, 3), ('8888', 'gamma', '1010102'), ('8', '10')),
    (4, 2, 4, (0, 1), ('8888', 'gamma'), ('8', '10')),
    (4, 2, 5, (2, 3), ('8888', 'gamma'), ('8', '10')),
]
EDGES = (0, 1, 2, 3, 31, 32, 62, 63, 64, 65, 94, 95, 96, 97, 126, 127, 128,
         129, 190, 191, 192, 193, 253, 254, 255)


def _bytes():
    """[pixel][sample][channel] bytes."""
    rng = random.Random(0x6A77A)
    out = []
    for p in range(WIDTH * HEIGHT):
        channels = []
        for ch in range(4):
            if p < 256:
                b = (p, 255 - p, (p * 7 + 3) & 255, (p * 13 + 5) & 255)[ch]
                channels.append((b,) * 4)
            elif p < 768:
                pick = (lambda: rng.choice(EDGES)) if rng.random() < 0.75 \
                    else (lambda: rng.randrange(256))
                a, b = pick(), pick()
                s = [b] * 4
                s[p % 4] = a
                channels.append(tuple(s))
            elif p < 1280:
                a, b = rng.randrange(256), rng.randrange(256)
                channels.append(((a, b, a, b), (a, a, b, b),
                                 (a, b, b, a))[p % 3])
            else:
                channels.append(tuple(rng.randrange(256) for _ in range(4)))
        out.append([tuple(channels[ch][k] for ch in range(4))
                    for k in range(4)])
    return out


BYTES = _bytes()


def tests():
    """The tests in order: {'name', 'kind': 'raw' or 'average', ...}."""
    out = []
    for samples, code in ((2, 1), (4, 2)):
        for k in range(samples):
            out.append({'name': f'{samples}x raw sample {k}', 'kind': 'raw',
                        'samples': samples, 'code': code, 'sample': k})
    for samples, code, select, chosen, views, dests in AVERAGES:
        for view in views:
            for dest in dests:
                out.append({
                    'name': f'{samples}x {"".join(map(str, chosen))} '
                            f'{view} {dest}',
                    'kind': 'average', 'samples': samples, 'code': code,
                    'sample_select': select, 'select': chosen, 'rt': view,
                    'dest': dest})
    return out


def build(suite):
    suite.set_inputs(rb.RECT, 2)
    for k in range(4):
        words = []
        for p in range(WIDTH * HEIGHT):
            words += [gpugen.f2w((v + 0.25) / 255) for v in BYTES[p][k]]
        suite.add_texture(f's {k}', words)
    for t in tests():
        n = t['samples']
        state = rb.registers(VIEWS['8888'], t['code'],
                             mode_cntl=gpugen.MODE_CNTL)
        passes = [([('PA_SC_AA_MASK', [0x1111 << k])], rb.fetch(f's {k}'),
                   rb.RECT_DRAW) for k in range(n - 1)]
        if t['kind'] == 'raw':
            resolve = gpugen.resolve_registers(
                WIDTH, HEIGHT, 6, 4, command=0, sample_select=t['sample'],
                registers=[('PA_SC_AA_MASK', [0xFFFF])])
        else:
            resolve = gpugen.resolve_registers(
                WIDTH, HEIGHT, DESTS[t['dest']], 4,
                sample_select=t['sample_select'],
                registers=[('RB_COLOR_INFO', [VIEWS[t['rt']] << 16]),
                           ('PA_SC_AA_MASK', [0xFFFF])])
        rb.test(suite, t['name'], f's {n - 1}', state, resolve,
                passes=passes,
                after_passes=[('PA_SC_AA_MASK', [0x1111 << (n - 1)])])
