#!/usr/bin/env python3
"""G-5 DD-label builder: reconstruct high-quality channel labels from the capture.

Pipeline: pairs.npz (from pair_capture.py) -> per pair:
  decoded TB bits -> CRC attach + segmentation -> LDPC encode (nr_ldpc) ->
  PUSCH rate matching (rv from rx_meta) -> scrambling -> modulation ->
  X_hat at the data REs -> H_dd = Y / X_hat -> FD+TD smoothing -> label grid.
The DMRS REs carry no data; the label there comes from the smoothed data-RE
estimates, so the (unrecorded) slot number is never needed: the data scrambling
c_init = n_rnti*2^15 + n_id has no slot term.
"""
import os, sys, csv
import numpy as np

# ---------------- CRC (38.212 5.1) ----------------
def _crc(bits, poly, n):
    reg = 0
    for b in bits:
        reg ^= b << (n - 1)
        for _ in range(1):
            pass
        # bitwise shift-register
        for _ in range(1):
            if reg & (1 << (n - 1)):
                reg = ((reg << 1) ^ poly) & ((1 << n) - 1)
            else:
                reg = (reg << 1) & ((1 << n) - 1)
    return reg

def crc_attach(bits, poly, n):
    """Append the CRC bits (MSB-first) to the bit list."""
    reg = 0
    out = []
    # Standard LFSR over the whole stream (zero-padded tail).
    stream = list(bits) + [0] * n
    for b in stream:
        fb = (reg >> (n - 1)) & 1
        reg = ((reg << 1) & ((1 << n) - 1)) | b
        if fb:
            reg ^= poly
        out.append(b)
    # The parity = the register after processing (MSB-first).
    crc_bits = [(reg >> (n - 1 - k)) & 1 for k in range(n)]
    return list(bits) + crc_bits

CRC16_POLY  = 0x1021
CRC24A_POLY = 0x1864CFB
CRC24B_POLY = 0x1800063

# ---------------- Segmentation (38.212 6.2.2) ----------------
def segment(tb_bits):
    """Returns a list of codeblock bit lists (with CRCs attached, fillers appended
    conceptually - fillers are encoded but not transmitted)."""
    if len(tb_bits) <= 3824:
        return [crc_attach(tb_bits, CRC16_POLY, 16)]
    b = len(tb_bits) + 24
    C = int(np.ceil(b / (8448 - 24)))
    b_plus = b + C * 24
    Kp = int(np.ceil(b_plus / C))
    out = []
    pos = 0
    tb = crc_attach(tb_bits, CRC24A_POLY, 24)
    for c in range(C):
        part = tb[c * Kp:(c + 1) * Kp]
        out.append(crc_attach(part, CRC24B_POLY, 24))
    return out

# ---------------- Rate matching (38.212 5.4.2) ----------------
def subblock_interleave(bits):
    """32-column sub-block interleaver with the NR pattern."""
    P = [0, 16, 8, 24, 4, 20, 12, 28, 2, 18, 10, 26, 6, 22, 14, 30,
         1, 17, 9, 25, 5, 21, 13, 29, 3, 19, 11, 27, 7, 23, 15, 31]
    D = len(bits)
    R = int(np.ceil(D / 32))
    N = R * 32
    nd = N - D
    y = np.zeros(N, dtype=np.uint8)
    k = 0
    for j in range(32):
        for i in range(R):
            idx = i * 32 + P[j]
            if idx < nd:
                continue
            y[idx - nd] = bits[k]
            k += 1
    return [int(v) for v in y]

def rate_match(codeblock_bits, K_transmitted, K_b, Zc, E, rv):
    """codeblock_bits: the full N-bit codeword from the LDPC encoder (punctured
    prefix first). K_b: the FULL information bit count incl. the fillers (the
    parity starts after them); K_transmitted: the info bits without the fillers
    (the systematic window excludes the trailing fillers). Returns E bits."""
    N = len(codeblock_bits)
    sys_bits  = codeblock_bits[2 * Zc:2 * Zc + K_transmitted]
    par_total = codeblock_bits[2 * Zc + K_b:]
    half      = len(par_total) // 2
    p0, p1    = par_total[:half], par_total[half:]
    s_i = subblock_interleave(sys_bits)
    p0_i = subblock_interleave(p0)
    p1_i = subblock_interleave(p1)
    buf = s_i + [v for pair in zip(p0_i, p1_i) for v in pair]
    Ncb = len(buf)
    k0 = {0: 0,
          1: (17 * Ncb // (32 * Zc)) * Zc,
          2: (33 * Ncb // (32 * Zc)) * Zc,
          3: (56 * Ncb // (32 * Zc)) * Zc}[rv]
    out = []
    k = k0
    while len(out) < E:
        out.append(buf[k % Ncb])
        k += 1
    return out

# ---------------- Scrambling (38.211 5.2.1 / 6.3.1.1) ----------------
def gold_seq(c_init, n):
    x1 = np.zeros(n + 1600, dtype=np.uint8); x1[0] = 1
    x2 = np.zeros(n + 1600, dtype=np.uint8)
    for i in range(n + 1600 - 31):
        x1[i + 31] = (x1[i + 3] ^ x1[i]) & 1
    for i in range(31):
        x2[i] = (c_init >> i) & 1
    for i in range(n + 1600 - 31):
        x2[i + 31] = (x2[i + 3] ^ x2[i + 2] ^ x2[i + 1] ^ x2[i]) & 1
    c = (x1[1600:1600 + n] ^ x2[1600:1600 + n])
    return c

def scramble(bits, c_init):
    c = gold_seq(c_init, len(bits))
    return [(b ^ int(cc)) for b, cc in zip(bits, c)]

# ---------------- Modulation (38.211 5.1) ----------------
def modulate(bits, mod):
    """mod = the srsRAN modulation_scheme enum value = BITS PER SYMBOL
    (QPSK=2, QAM16=4, QAM64=6, QAM256=8)."""
    if mod == 2:  # QPSK
        syms = []
        for i in range(0, len(bits), 2):
            b0, b1 = bits[i], bits[i + 1]
            syms.append(((1 - 2 * b0) + 1j * (1 - 2 * b1)) / np.sqrt(2))
        return syms
    if mod == 4:  # 16QAM
        syms = []
        for i in range(0, len(bits), 4):
            b0, b1, b2, b3 = bits[i:i + 4]
            re = (1 - 2 * b0) * (2 - (1 - 2 * b2))
            im = (1 - 2 * b1) * (2 - (1 - 2 * b3))
            syms.append((re + 1j * im) / np.sqrt(10))
        return syms
    if mod == 6:  # 64QAM
        syms = []
        for i in range(0, len(bits), 6):
            b0, b1, b2, b3, b4, b5 = bits[i:i + 6]
            re = (1 - 2 * b0) * (4 - (1 - 2 * b2) * (2 - (1 - 2 * b4)))
            im = (1 - 2 * b1) * (4 - (1 - 2 * b3) * (2 - (1 - 2 * b5)))
            syms.append((re + 1j * im) / np.sqrt(42))
        return syms
    if mod == 8:  # 256QAM
        syms = []
        for i in range(0, len(bits), 8):
            b = bits[i:i + 8]
            re = (1 - 2 * b[0]) * (8 - (1 - 2 * b[2]) * (4 - (1 - 2 * b[4]) * (2 - (1 - 2 * b[6]))))
            im = (1 - 2 * b[1]) * (8 - (1 - 2 * b[3]) * (4 - (1 - 2 * b[5]) * (2 - (1 - 2 * b[7]))))
            syms.append((re + 1j * im) / np.sqrt(170))
        return syms
    raise ValueError(f'bad mod {mod}')

# ---------------- FD/TD smoothing (reuse the dataset-generator semantics) ----------------
def smooth_dense(H, n_sc, n_sym, dmrs_mask, prb):
    """H: [n_sc, n_sym] complex, valid at ALL the subcarriers of the NON-DMRS
    symbols (the direct DD estimates); the DMRS symbols carry no data and are
    filled by the temporal interpolation of their neighbouring symbols."""
    out = H.copy()
    for sym in range(n_sym):
        if not dmrs_mask[sym]:
            continue
        lo = sym - 1 if sym - 1 >= 0 else sym + 2
        hi = sym + 1 if sym + 1 < n_sym else sym - 2
        if hi >= n_sym:
            hi = lo
        out[:, sym] = 0.5 * (out[:, lo] + out[:, hi])
    return out

if __name__ == '__main__':
    print('dd_label module: self-tests')
    # CRC sanity: the appended parity must be nonzero for a nonzero input and the
    # zero input must produce the zero parity.
    assert crc_attach([0]*16, CRC16_POLY, 16)[16:] == [0]*16
    assert any(crc_attach([1]*16, CRC16_POLY, 16)[16:])
    print('crc ok')
    # Scrambling orthogonality.
    a = scramble([1]*64, 0); b = scramble([1]*64, 1)
    print('scramble differs:', a != b)
    # Modulation energy (random bits -> the average symbol energy must be ~1).
    rng = np.random.default_rng(0)
    for mod in (2, 4, 6, 8):
        s = modulate(list(rng.integers(0, 2, 1536)), mod)
        e = np.mean(np.abs(np.array(s))**2)
        print(f'mod {mod} energy: {e:.3f} (expect ~1)')
