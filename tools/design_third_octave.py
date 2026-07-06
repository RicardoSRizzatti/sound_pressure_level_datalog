"""Design the multirate 1/3-octave filter bank (IEC 61260-style) for fs=48 kHz.

Architecture (like real sound level analyzers):
  - 5 rate stages; each lower stage is decimated by 4 from the previous one
    (8th-order Butterworth low-pass anti-alias, 4 biquads).
  - Each band is a 6th-order Butterworth band-pass (3 biquads) running at the
    stage rate, so fc/fs stays in a numerically friendly range for float32.

Stages (fs -> bands):
  48000 Hz : 2.5k 3.15k 4k 5k 6.3k 8k 10k 12.5k 16k   (9)
  12000 Hz : 630 800 1k 1.25k 1.6k 2k                  (6)
   3000 Hz : 160 200 250 315 400 500                   (6)
    750 Hz : 40 50 63 80 100 125                       (6)
  187.5 Hz : 20 25 31.5                                (3)

Outputs src/spl_toct_coeffs.h and prints a verification table (center gain,
band-edge gain, one-octave-out attenuation) against practical class-2 goals.

Usage: python design_third_octave.py [--header out.h]
"""
import argparse
import sys

import numpy as np
from scipy import signal

FS = 48000.0
DECIM = 4
G = 10 ** 0.3   # base-10 octave ratio (IEC 61260)

# Nominal labels and exact base-10 center frequencies fm = 1000 * G^(x/3).
BANDS = [
    (20, -17), (25, -16), (31.5, -15), (40, -14), (50, -13), (63, -12),
    (80, -11), (100, -10), (125, -9), (160, -8), (200, -7), (250, -6),
    (315, -5), (400, -4), (500, -3), (630, -2), (800, -1), (1000, 0),
    (1250, 1), (1600, 2), (2000, 3), (2500, 4), (3150, 5), (4000, 6),
    (5000, 7), (6300, 8), (8000, 9), (10000, 10), (12500, 11), (16000, 12),
]

# Stage assignment: nominal label -> stage index (0 = full rate).
STAGE_OF = {}
for label, _ in BANDS:
    if label >= 2500:
        STAGE_OF[label] = 0
    elif label >= 630:
        STAGE_OF[label] = 1
    elif label >= 160:
        STAGE_OF[label] = 2
    elif label >= 40:
        STAGE_OF[label] = 3
    else:
        STAGE_OF[label] = 4

N_STAGES = 5


def stage_fs(stage: int) -> float:
    return FS / (DECIM ** stage)


def design_band(fm: float, fs: float):
    """6th-order Butterworth band-pass (3 SOS) with IEC edges fm*G^(±1/6)."""
    lo = fm * G ** (-1 / 6)
    hi = fm * G ** (1 / 6)
    return signal.butter(3, [lo, hi], btype="bandpass", fs=fs, output="sos")


def design_decimator(fs_in: float):
    """10th-order Butterworth low-pass (5 SOS) for ÷4 decimation."""
    cutoff = (fs_in / DECIM) * 0.40
    return signal.butter(10, cutoff, btype="lowpass", fs=fs_in, output="sos")


def sos_to_c(sos: np.ndarray, indent: str = "    ") -> str:
    """Emit CMSIS-DSP df2T layout: {b0, b1, b2, -a1, -a2} — usable straight
    from flash by arm_biquad_cascade_df2T_init_f32 (no RAM copy)."""
    out = []
    for b0, b1, b2, a0, a1, a2 in sos:
        out.append(
            f"{indent}{{ {b0:+.9e}f, {b1:+.9e}f, {b2:+.9e}f, "
            f"{-a1:+.9e}f, {-a2:+.9e}f }},"
        )
    return "\n".join(out)


def response_db(sos: np.ndarray, freqs, fs: float) -> np.ndarray:
    w, h = signal.sosfreqz(sos, worN=np.asarray(freqs, dtype=float), fs=fs)
    return 20 * np.log10(np.maximum(np.abs(h), 1e-12))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--header", default=None, help="caminho do .h gerado")
    args = parser.parse_args()

    bands = []
    for label, x in BANDS:
        fm = 1000.0 * G ** (x / 3.0)
        stage = STAGE_OF[label]
        fs = stage_fs(stage)
        sos = design_band(fm, fs)
        bands.append((label, fm, stage, fs, sos))

    decimators = [design_decimator(stage_fs(s)) for s in range(N_STAGES - 1)]

    # ---- Verification ---------------------------------------------------------
    print("// banda   fm(Hz)  estagio  g(fm)dB  g(borda-)dB  g(borda+)dB  g(fm/2)dB  g(2fm)dB")
    ok = True
    for label, fm, stage, fs, sos in bands:
        lo = fm * G ** (-1 / 6)
        hi = fm * G ** (1 / 6)
        pts = [fm, lo, hi, fm / 2, min(2 * fm, fs * 0.499)]
        g = response_db(sos, pts, fs)
        flag = ""
        # Metas práticas (classe 2): centro ±0,5 dB; bordas ~-3 dB (janela -5..+0,5);
        # uma oitava fora: <= -40 dB (fm/2 sempre; 2fm quando dentro de Nyquist).
        if abs(g[0]) > 0.5:
            flag += " CENTRO!"
        if not (-5.0 <= g[1] <= 0.5) or not (-5.0 <= g[2] <= 0.5):
            flag += " BORDA!"
        if g[3] > -40.0:
            flag += " OITAVA-!"
        if 2 * fm < fs * 0.499 and g[4] > -40.0:
            flag += " OITAVA+!"
        if flag:
            ok = False
        print(f"//  {label:>6}  {fm:8.1f}  {stage}  {g[0]:+7.2f}  {g[1]:+8.2f}  "
              f"{g[2]:+8.2f}  {g[3]:+8.1f}  {g[4]:+8.1f} {flag}")

    # Decimators: flat where the next stage's top band lives, strong at fold zone.
    for s, dec in enumerate(decimators):
        fs_in = stage_fs(s)
        fs_out = fs_in / DECIM
        top_edge = max(fm * G ** (1 / 6) for label, fm, st, _, _ in bands if st == s + 1)
        fold_lo = fs_out - top_edge   # aliases from here fold onto the top band
        g = response_db(dec, [top_edge, fold_lo], fs_in)
        flag = ""
        if abs(g[0]) > 0.2:
            flag += " PASSBAND!"
        if g[1] > -60.0:
            flag += " ALIAS!"
        if flag:
            ok = False
        print(f"// decim {s}->{s+1} (fs {fs_in:.0f}->{fs_out:.1f}): "
              f"g({top_edge:.0f} Hz)={g[0]:+.3f} dB, g(alias {fold_lo:.0f} Hz)={g[1]:+.1f} dB{flag}")

    print(f"// Resultado: {'OK' if ok else 'REVISAR'}")

    # ---- Header ---------------------------------------------------------------
    if args.header:
        lines = []
        lines.append("// 1/3-octave multirate filter bank, fs = 48000 Hz.")
        lines.append("// Generated by tools/design_third_octave.py - do not edit by hand.")
        lines.append("// Biquad layout per section: { b0, b1, b2, -a1, -a2 } (CMSIS df2T-ready).")
        lines.append("#ifndef SPL_TOCT_COEFFS_H")
        lines.append("#define SPL_TOCT_COEFFS_H")
        lines.append("")
        lines.append(f"#define SPL_TOCT_NUM_BANDS {len(bands)}")
        lines.append(f"#define SPL_TOCT_NUM_STAGES {N_STAGES}")
        lines.append("#define SPL_TOCT_BAND_SECTIONS 3")
        lines.append("#define SPL_TOCT_DECIM_SECTIONS 5")
        lines.append(f"#define SPL_TOCT_DECIM_FACTOR {DECIM}")
        lines.append("")
        labels = ", ".join(str(int(label * 10)) for label, *_ in bands)
        lines.append("// Nominal center frequency of each band, in tenths of Hz (315 = 31.5 Hz).")
        lines.append(f"static const uint32_t spl_toct_band_cfreq_dhz[SPL_TOCT_NUM_BANDS] = {{ {labels} }};")
        stages = ", ".join(str(stage) for _, _, stage, _, _ in bands)
        lines.append(f"static const uint8_t spl_toct_band_stage[SPL_TOCT_NUM_BANDS] = {{ {stages} }};")
        lines.append("")
        lines.append("// Band-pass sections, bands in the order above (3 sections each).")
        lines.append("static const float spl_toct_band_sos[SPL_TOCT_NUM_BANDS * SPL_TOCT_BAND_SECTIONS][5] = {")
        for label, fm, stage, fs, sos in bands:
            lines.append(f"  // {label} Hz (fm {fm:.1f} Hz, estagio {stage}, fs {fs:.1f} Hz)")
            lines.append(sos_to_c(sos, "  "))
        lines.append("};")
        lines.append("")
        lines.append("// Anti-alias low-pass sections for each /4 decimation stage (4 each).")
        lines.append("static const float spl_toct_decim_sos[(SPL_TOCT_NUM_STAGES - 1) * SPL_TOCT_DECIM_SECTIONS][5] = {")
        for s, dec in enumerate(decimators):
            lines.append(f"  // decimador estagio {s} -> {s + 1}")
            lines.append(sos_to_c(dec, "  "))
        lines.append("};")
        lines.append("")
        lines.append("#endif // SPL_TOCT_COEFFS_H")
        with open(args.header, "w", encoding="ascii") as f:
            f.write("\n".join(lines) + "\n")
        print(f"// header gravado em {args.header}")

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
