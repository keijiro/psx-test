pub(crate) const WAVE_SAMPLE_COUNT: usize = 56;
pub(crate) const DEPTH_COUNT: usize = 11;
pub(crate) const SHAPE_COUNT: usize = 5;
pub(crate) const WAVE_COUNT: usize = DEPTH_COUNT * SHAPE_COUNT;
pub(crate) const SINE: [i16; WAVE_SAMPLE_COUNT] = [
    0, 1605, 3190, 4735, 6220, 7627, 8938, 10137, 11208, 12139, 12916, 13532, 13977, 14246, 14336,
    14246, 13977, 13532, 12916, 12139, 11208, 10137, 8938, 7627, 6220, 4735, 3190, 1605, 0, -1605,
    -3190, -4735, -6220, -7627, -8938, -10137, -11208, -12139, -12916, -13532, -13977, -14246,
    -14336, -14246, -13977, -13532, -12916, -12139, -11208, -10137, -8938, -7627, -6220, -4735,
    -3190, -1605,
];

pub(crate) fn wave_index(shape: usize, depth: usize) -> usize {
    shape * DEPTH_COUNT + depth
}

fn shape_sample(shape: usize, phase: usize) -> i32 {
    const PEAK: i32 = 14336;
    match shape {
        0 => SINE[phase] as i32,
        1 => {
            if phase < WAVE_SAMPLE_COUNT / 2 {
                PEAK
            } else {
                -PEAK
            }
        }
        2 => PEAK - 2 * PEAK * phase as i32 / WAVE_SAMPLE_COUNT as i32,
        3 => {
            let quarter = WAVE_SAMPLE_COUNT / 4;
            if phase < quarter {
                4 * PEAK * phase as i32 / WAVE_SAMPLE_COUNT as i32
            } else if phase < 3 * quarter {
                2 * PEAK - 4 * PEAK * phase as i32 / WAVE_SAMPLE_COUNT as i32
            } else {
                4 * PEAK * phase as i32 / WAVE_SAMPLE_COUNT as i32 - 4 * PEAK
            }
        }
        // A fixed loop makes noise repeatable across depth and pitch changes.
        4 => {
            let mut value = (phase as u32 + 1).wrapping_mul(0x9e37_79b9);
            value ^= value >> 16;
            value = value.wrapping_mul(0x85eb_ca6b);
            value ^= value >> 13;
            (value as i32 >> 17) * PEAK / 16384
        }
        _ => unreachable!(),
    }
}

pub(crate) fn build_samples(waves: &mut [[i16; WAVE_SAMPLE_COUNT]; WAVE_COUNT]) {
    // PMOD reads the previous voice before its left/right mixer volume, so
    // scaled samples give depth control while that voice remains inaudible.
    for (index, wave) in waves.iter_mut().enumerate() {
        let depth = index % DEPTH_COUNT;
        let shape = index / DEPTH_COUNT;
        // The last two steps approach the signed 16-bit sample limit without
        // wrapping.
        let peak = if depth <= 8 {
            3584 * depth as i32
        } else {
            28672 + 2047 * (depth as i32 - 8)
        };
        let mut source = [0i32; WAVE_SAMPLE_COUNT];
        for (phase, sample) in source.iter_mut().enumerate() {
            *sample = shape_sample(shape, phase);
        }
        if shape == 2 || shape == 4 {
            // PMOD turns DC into a pitch offset. Center the complete loop and
            // normalize afterwards so subtracting noise's mean cannot overflow
            // at high depths. Keep the fractional mean until the final division.
            let sum: i32 = source.iter().sum();
            for sample in &mut source {
                *sample = *sample * WAVE_SAMPLE_COUNT as i32 - sum;
            }
            let maximum = source.iter().map(|sample| sample.abs()).max().unwrap();
            for (sample, source) in wave.iter_mut().zip(source) {
                *sample = (source as i64 * peak as i64 / maximum as i64) as i16;
            }
        } else {
            for (sample, source) in wave.iter_mut().zip(source) {
                *sample = (source * peak / 14336) as i16;
            }
        }
        // Integer scaling can leave a fractional-sample mean. Spread its
        // remainder over the loop rather than changing one sample sharply.
        let mut remainder: i32 = wave.iter().map(|&sample| sample as i32).sum();
        for sample in wave.iter_mut() {
            let correction = remainder.signum();
            *sample = (*sample as i32 - correction) as i16;
            remainder -= correction;
        }
        assert_eq!(remainder, 0);
    }
}

#[cfg(not(target_arch = "mips"))]
pub(crate) fn build_waves(
    waves: &mut [[i16; WAVE_SAMPLE_COUNT]; WAVE_COUNT],
    encoded: &mut [[u8; 32]; WAVE_COUNT],
) {
    build_samples(waves);
    for (samples, output) in waves.iter().zip(encoded.iter_mut()) {
        crate::adpcm::encode_loop(output, samples);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn generated_adpcm_matches_build_output() {
        let mut waves = [[0i16; WAVE_SAMPLE_COUNT]; WAVE_COUNT];
        let mut encoded = [0u8; WAVE_COUNT * 32];
        unsafe {
            crate::synth_build_waves(waves.as_mut_ptr() as *mut i16, encoded.as_mut_ptr());
        }
        assert!(waves[0].iter().all(|&sample| sample == 0));
        assert_eq!(waves[4], SINE);
        assert_eq!(waves[8][14], 28672);
        assert_eq!(waves[10][14], 32766);
        for shape in 0..SHAPE_COUNT {
            assert!(waves[wave_index(shape, 0)]
                .iter()
                .all(|&sample| sample == 0));
            if shape != 2 && shape != 4 {
                for index in 0..WAVE_SAMPLE_COUNT {
                    assert_eq!(
                        waves[wave_index(shape, 4)][index],
                        shape_sample(shape, index) as i16
                    );
                }
            }
        }
        for wave in 0..WAVE_COUNT {
            assert_eq!(encoded[wave * 32 + 1], 4);
            assert_eq!(encoded[wave * 32 + 17], 3);
        }
        let mut expected_samples = [[0i16; WAVE_SAMPLE_COUNT]; WAVE_COUNT];
        let mut expected_adpcm = [[0u8; 32]; WAVE_COUNT];
        build_waves(&mut expected_samples, &mut expected_adpcm);
        assert_eq!(waves, expected_samples);
        for (wave, block) in expected_adpcm.iter().enumerate() {
            assert_eq!(&encoded[wave * 32..][..32], block);
        }
    }

    #[test]
    fn all_source_loops_are_centered_and_bounded() {
        let mut waves = [[0i16; WAVE_SAMPLE_COUNT]; WAVE_COUNT];
        build_samples(&mut waves);
        for (index, wave) in waves.iter().enumerate() {
            assert_eq!(
                wave.iter().map(|&sample| sample as i32).sum::<i32>(),
                0,
                "source wave {index}"
            );
            let depth = index % DEPTH_COUNT;
            let peak = if depth <= 8 {
                3584 * depth as i32
            } else {
                28672 + 2047 * (depth as i32 - 8)
            };
            assert!(wave.iter().all(|&sample| (sample as i32).abs() <= peak));
        }
    }

    #[test]
    fn fixed_adpcm_loops_keep_pitch_centered() {
        let mut waves = [[0i16; WAVE_SAMPLE_COUNT]; WAVE_COUNT];
        let mut encoded = [[0u8; 32]; WAVE_COUNT];
        build_waves(&mut waves, &mut encoded);
        for (index, data) in encoded.iter().enumerate() {
            // Decode the bytes independently, retaining history through actual
            // loop jumps. The encoder's own error estimate is not sufficient.
            let coefficients = [(0, 0), (60, 0), (115, -52), (98, -55), (122, -60)];
            let mut history = [0i32; 2];
            let mut decoded = [0i32; WAVE_SAMPLE_COUNT];
            let mut total = 0i64;
            for cycle in 0..256 {
                for block in 0..2 {
                    let header = data[block * 16];
                    let (a, b) = coefficients[(header >> 4) as usize];
                    for sample in 0..28 {
                        let byte = data[block * 16 + 2 + sample / 2];
                        let nibble = ((byte >> (sample % 2 * 4)) & 15) as i32;
                        let residual = (if nibble < 8 { nibble } else { nibble - 16 })
                            * (4096 >> (header & 15));
                        let value = (residual + ((a * history[0] + b * history[1] + 32) >> 6))
                            .clamp(-32768, 32767);
                        history = [value, history[0]];
                        decoded[block * 28 + sample] = value;
                    }
                }
                if cycle >= 128 {
                    total += decoded.iter().map(|&sample| sample as i64).sum::<i64>();
                }
            }
            assert!(
                total.abs() <= (128 * WAVE_SAMPLE_COUNT) as i64,
                "decoded wave {index}: sum {total}"
            );
            if index % DEPTH_COUNT == 0 {
                assert!(decoded.iter().all(|&sample| sample == 0));
            }
            // The corrected bank's worst RMS reconstruction errors are 0.41%
            // for sine and 1.63% for triangle. Limits of 1% and 2% guard against
            // losing predictor quality when the DC search changes.
            if index / DEPTH_COUNT == 0 || index / DEPTH_COUNT == 3 {
                let error: i64 = decoded
                    .iter()
                    .zip(waves[index])
                    .map(|(&actual, target)| {
                        let difference = actual as i64 - target as i64;
                        difference * difference
                    })
                    .sum();
                let energy: i64 = waves[index]
                    .iter()
                    .map(|&sample| sample as i64 * sample as i64)
                    .sum();
                let limit = if index / DEPTH_COUNT == 0 {
                    10000
                } else {
                    2500
                };
                assert!(
                    error * limit <= energy,
                    "waveform error at wave {index}: {error}/{energy}"
                );
            }
        }
    }
}
