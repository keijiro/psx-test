use crate::waveform_config::{DEPTH_COUNT, SHAPE_COUNT, WAVE_COUNT, WAVE_SAMPLE_COUNT};
use std::f64::consts::TAU;

const PEAK: i32 = 14336;
pub(crate) const SINE: [i16; WAVE_SAMPLE_COUNT] = [
    0, 1605, 3190, 4735, 6220, 7627, 8938, 10137, 11208, 12139, 12916, 13532, 13977, 14246, 14336,
    14246, 13977, 13532, 12916, 12139, 11208, 10137, 8938, 7627, 6220, 4735, 3190, 1605, 0, -1605,
    -3190, -4735, -6220, -7627, -8938, -10137, -11208, -12139, -12916, -13532, -13977, -14246,
    -14336, -14246, -13977, -13532, -12916, -12139, -11208, -10137, -8938, -7627, -6220, -4735,
    -3190, -1605,
];

fn triangle(phase: f64, apex: f64) -> f64 {
    if phase < apex {
        -1.0 + 2.0 * phase / apex
    } else {
        1.0 - 2.0 * (phase - apex) / (1.0 - apex)
    }
}

fn partials(phase: f64, harmonics: &[(usize, f64)]) -> f64 {
    harmonics
        .iter()
        .map(|&(harmonic, level)| level * (TAU * harmonic as f64 * phase).sin())
        .sum()
}

fn saw_partials(phase: f64, keep: impl Fn(usize) -> bool) -> f64 {
    // A 56-sample period has no distinct harmonics above 27. Leave out the
    // Nyquist bin so sine-phase partials do not depend on rounding sin(n*pi).
    (1..WAVE_SAMPLE_COUNT / 2)
        .filter(|&harmonic| keep(harmonic))
        .map(|harmonic| (TAU * harmonic as f64 * phase).sin() / harmonic as f64)
        .sum()
}

fn resonant_saw(phase: f64, center: f64) -> f64 {
    (1..WAVE_SAMPLE_COUNT / 2)
        .map(|harmonic| {
            let distance = (harmonic as f64 - center) / 0.8;
            let level = 1.0 / harmonic as f64 + 0.8 * (-0.5 * distance * distance).exp();
            level * (TAU * harmonic as f64 * phase).sin()
        })
        .sum()
}

fn vowel(phase: f64, centers: [f64; 2]) -> f64 {
    // Harmonic-space peaks approximate vowels around a 160 Hz reference note.
    // Widths retain neighboring partials instead of turning each formant into
    // a pure tone. Resampling moves both peaks with the played pitch.
    (1..WAVE_SAMPLE_COUNT / 2)
        .map(|harmonic| {
            let harmonic = harmonic as f64;
            let low = (harmonic - centers[0]) / 0.8;
            let high = (harmonic - centers[1]) / 1.2;
            let level = (0.12 + 2.0 * (-0.5 * low * low).exp() + 1.4 * (-0.5 * high * high).exp())
                / harmonic;
            level * (TAU * harmonic * phase).sin()
        })
        .sum()
}

fn noise(phase: usize) -> f64 {
    // Keep the original fixed sequence so all depths share the same texture.
    let mut value = (phase as u32 + 1).wrapping_mul(0x9e37_79b9);
    value ^= value >> 16;
    value = value.wrapping_mul(0x85eb_ca6b);
    value ^= value >> 13;
    ((value as i32 >> 17) * PEAK / 16384) as f64 / PEAK as f64
}

fn shape_sample(shape: usize, sample: usize) -> f64 {
    let phase = sample as f64 / WAVE_SAMPLE_COUNT as f64;
    let sine = (TAU * phase).sin();
    let a = [4.0, 10.0];
    let e = [3.0, 13.0];
    let o = [3.0, 6.0];
    let u = [2.0, 8.0];
    match shape {
        // Preserve the original integer sine, triangle, saw, and square shapes.
        0 => SINE[sample] as f64 / PEAK as f64,
        1 => {
            let quarter = WAVE_SAMPLE_COUNT / 4;
            let value = if sample < quarter {
                4 * PEAK * sample as i32 / WAVE_SAMPLE_COUNT as i32
            } else if sample < 3 * quarter {
                2 * PEAK - 4 * PEAK * sample as i32 / WAVE_SAMPLE_COUNT as i32
            } else {
                4 * PEAK * sample as i32 / WAVE_SAMPLE_COUNT as i32 - 4 * PEAK
            };
            value as f64 / PEAK as f64
        }
        2 => (PEAK - 2 * PEAK * sample as i32 / WAVE_SAMPLE_COUNT as i32) as f64 / PEAK as f64,
        3 => {
            if phase < 0.5 {
                1.0
            } else {
                -1.0
            }
        }
        4..=6 => {
            if phase < [0.125, 0.25, 0.375][shape - 4] {
                1.0
            } else {
                -1.0
            }
        }
        7 => triangle(phase, 0.25),
        8 => partials(phase, &[(1, 1.0), (2, 0.5)]),
        9 => partials(phase, &[(1, 1.0), (3, 0.5)]),
        10 => partials(phase, &[(1, 1.0), (5, 0.35)]),
        11 => partials(phase, &[(1, 1.0), (7, 0.25)]),
        12 => partials(phase, &[(1, 1.0), (3, 0.7), (5, 0.5), (7, 0.3)]),
        13 => saw_partials(phase, |n| n == 1 || (n <= 8 && n % 2 == 0)),
        14 => partials(phase, &[(1, 1.0), (2, 0.7), (4, 0.5), (8, 0.3)]),
        15 => saw_partials(phase, |n| [1, 2, 3, 5, 7, 11].contains(&n)),
        16 => saw_partials(phase, |n| n <= 4),
        17 => saw_partials(phase, |n| n <= 8),
        18 => saw_partials(phase, |n| n <= 7 && n % 2 == 1),
        19 => saw_partials(phase, |n| !(3..=6).contains(&n)),
        20..=22 => resonant_saw(phase, [3.0, 7.0, 13.0][shape - 20]),
        23 => saw_partials(phase, |n| n <= 13 && n % 3 == 1),
        24 => sine.max(0.0),
        25 => {
            if phase < 0.25 {
                sine
            } else {
                0.0
            }
        }
        26 => sine * sine.abs(),
        27 => sine.clamp(-0.75, 0.75),
        28 => sine.clamp(-0.35, 0.35),
        29 => sine.clamp(-0.25, 0.75),
        // A triangular transfer curve reflects the signal at +/-1 rather
        // than wrapping it, retaining the continuous wavefolder shape.
        30..=31 => {
            let driven = sine * [2.0, 4.0][shape - 30];
            1.0 - ((driven + 1.0).rem_euclid(4.0) - 2.0).abs()
        }
        32..=33 => {
            let split = [0.25, 0.125][shape - 32];
            let bent = if phase < split {
                0.5 * phase / split
            } else {
                0.5 + 0.5 * (phase - split) / (1.0 - split)
            };
            (TAU * bent).sin()
        }
        34..=39 => {
            let (ratio, index) = [
                (1.0, 0.8),
                (2.0, 0.8),
                (3.0, 0.8),
                (4.0, 0.8),
                (2.0, 2.0),
                (3.0, 2.0),
            ][shape - 34];
            (TAU * phase + index * (TAU * ratio * phase).sin()).sin()
        }
        // Fractional slave ratios prevent these from being just a higher
        // octave. Every table boundary resets the slave to the master period.
        40..=42 => 1.0 - 2.0 * (phase * [1.5, 2.5, 3.5][shape - 40]).fract(),
        43..=44 => (TAU * phase * [1.5, 2.5][shape - 43]).sin(),
        45 => triangle((phase * 2.5).fract(), 0.5),
        46..=47 => (TAU * phase * [3.0, 7.0][shape - 46]).sin() * (-4.0 * phase).exp(),
        48 => vowel(phase, a),
        49 => vowel(phase, e),
        50 => vowel(phase, [2.0, 17.0]),
        51 => vowel(phase, o),
        52 => vowel(phase, u),
        53 => 0.5 * (vowel(phase, a) + vowel(phase, e)),
        54 => 0.5 * (vowel(phase, o) + vowel(phase, u)),
        55 => 0.7 * sine + vowel(phase, [5.0, 9.0]),
        56 => 1.0 - 2.0 * (sample * 8 / WAVE_SAMPLE_COUNT) as f64 / 7.0,
        57 => ((triangle(phase, 0.5) + 1.0) * 3.5).round() / 3.5 - 1.0,
        58 => ((sine + 1.0) * 3.5).round() / 3.5 - 1.0,
        59 => (TAU * (sample * 8 / WAVE_SAMPLE_COUNT) as f64 / 8.0).sin(),
        60 => noise(sample),
        // Smooth across the loop boundary too, avoiding a new seam in the
        // low-pass texture. Sample-held noise uses exactly eight seven-sample
        // plateaus, while binary noise retains the full-rate fixed sequence.
        61 => {
            (noise((sample + WAVE_SAMPLE_COUNT - 1) % WAVE_SAMPLE_COUNT)
                + 2.0 * noise(sample)
                + noise((sample + 1) % WAVE_SAMPLE_COUNT))
                / 4.0
        }
        62 => {
            if noise(sample) >= 0.0 {
                1.0
            } else {
                -1.0
            }
        }
        63 => noise(sample * 8 / WAVE_SAMPLE_COUNT),
        _ => unreachable!(),
    }
}

pub(crate) fn build_samples(waves: &mut [[i16; WAVE_SAMPLE_COUNT]; WAVE_COUNT]) {
    for shape in 0..SHAPE_COUNT {
        let mut source = [0i32; WAVE_SAMPLE_COUNT];
        for (phase, sample) in source.iter_mut().enumerate() {
            let value = shape_sample(shape, phase);
            assert!(value.is_finite());
            *sample = (value * PEAK as f64).round() as i32;
        }
        // PMOD turns DC into a pitch offset. Center every complete loop, then
        // normalize so asymmetric pulses and rectified waves cannot overflow
        // at high depths. Retain the fractional mean until the final division.
        let sum: i32 = source.iter().sum();
        for sample in &mut source {
            *sample = *sample * WAVE_SAMPLE_COUNT as i32 - sum;
        }
        let maximum = source.iter().map(|sample| sample.abs()).max().unwrap();
        assert!(maximum > 0);
        for depth in 0..DEPTH_COUNT {
            // Scaled samples control PMOD before the muted voice's mixer
            // volume. The last two steps approach the signed 16-bit limit.
            let peak = if depth <= 8 {
                3584 * depth as i32
            } else {
                28672 + 2047 * (depth as i32 - 8)
            };
            let wave = &mut waves[shape * DEPTH_COUNT + depth];
            for (sample, &source) in wave.iter_mut().zip(&source) {
                *sample = (source as i64 * peak as i64 / maximum as i64) as i16;
            }
            // Spread integer scaling's remainder instead of creating a sharp
            // correction in one sample. Skip saturated samples when a one-unit
            // correction would cross the depth's peak limit.
            let mut remainder: i32 = wave.iter().map(|&sample| sample as i32).sum();
            for sample in wave.iter_mut() {
                let corrected = *sample as i32 - remainder.signum();
                if corrected.abs() <= peak {
                    remainder -= remainder.signum();
                    *sample = corrected as i16;
                }
            }
            assert_eq!(remainder, 0, "shape {shape}, depth {depth}");
        }
    }
}

pub(crate) fn build_waves(
    waves: &mut [[i16; WAVE_SAMPLE_COUNT]; WAVE_COUNT],
    encoded: &mut [[u8; 32]; WAVE_COUNT],
) {
    build_samples(waves);
    for (samples, output) in waves.iter().zip(encoded.iter_mut()) {
        crate::adpcm::encode_loop(output, samples);
    }
}
