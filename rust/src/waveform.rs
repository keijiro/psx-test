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
        1 => if phase < WAVE_SAMPLE_COUNT / 2 { PEAK } else { -PEAK },
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
        // wrapping. Depths 0..8 keep their previous amplitudes.
        let peak = if depth <= 8 {
            3584 * depth as i32
        } else {
            28672 + 2047 * (depth as i32 - 8)
        };
        for (sample_index, sample) in wave.iter_mut().enumerate() {
            *sample = (shape_sample(shape, sample_index) * peak / 14336) as i16;
        }
    }
}

#[cfg(not(target_arch = "mips"))]
pub(crate) fn build_waves(
    waves: &mut [[i16; WAVE_SAMPLE_COUNT]; WAVE_COUNT],
    encoded: &mut [[u8; 32]; WAVE_COUNT],
) {
    build_samples(waves);
    for (samples, output) in waves.iter().zip(encoded.iter_mut()) {
        let mut encoder = crate::adpcm::AdpcmEncoder::default();
        // Carry predictor state across passes to converge on the loop boundary.
        for _ in 0..8 {
            for block in 0..2 {
                let flags = if block == 0 { 0x04 } else { 0x03 };
                let dest = (&mut output[block * 16..][..16]).try_into().unwrap();
                let samples = (&samples[block * 28..][..28]).try_into().unwrap();
                encoder.encode_block(dest, samples, flags);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn generated_adpcm_matches_original_output() {
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
            assert!(waves[wave_index(shape, 0)].iter().all(|&sample| sample == 0));
            for index in 0..WAVE_SAMPLE_COUNT {
                assert_eq!(waves[wave_index(shape, 4)][index], shape_sample(shape, index) as i16);
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
}
