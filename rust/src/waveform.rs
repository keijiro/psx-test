pub(crate) const WAVE_SAMPLE_COUNT: usize = 56;
pub(crate) const DEPTH_COUNT: usize = 11;
pub(crate) const CYCLES: [usize; 3] = [1, 2, 4];
pub(crate) const WAVE_COUNT: usize = DEPTH_COUNT * CYCLES.len();
pub(crate) const SINE: [i16; WAVE_SAMPLE_COUNT] = [
    0, 1605, 3190, 4735, 6220, 7627, 8938, 10137, 11208, 12139, 12916, 13532, 13977, 14246, 14336,
    14246, 13977, 13532, 12916, 12139, 11208, 10137, 8938, 7627, 6220, 4735, 3190, 1605, 0, -1605,
    -3190, -4735, -6220, -7627, -8938, -10137, -11208, -12139, -12916, -13532, -13977, -14246,
    -14336, -14246, -13977, -13532, -12916, -12139, -11208, -10137, -8938, -7627, -6220, -4735,
    -3190, -1605,
];

fn build_samples(waves: &mut [[i16; WAVE_SAMPLE_COUNT]; WAVE_COUNT]) {
    // PMOD reads the previous voice before its left/right mixer volume, so
    // scaled samples give depth control while that voice remains inaudible.
    for (index, wave) in waves.iter_mut().enumerate() {
        let depth = index % DEPTH_COUNT;
        let cycles = CYCLES[index / DEPTH_COUNT];
        // The last two steps approach the signed 16-bit sample limit without
        // wrapping. Depths 0..8 keep their previous amplitudes.
        let peak = if depth <= 8 {
            3584 * depth as i32
        } else {
            28672 + 2047 * (depth as i32 - 8)
        };
        for (sample_index, sample) in wave.iter_mut().enumerate() {
            let sine = SINE[(sample_index * cycles) % WAVE_SAMPLE_COUNT];
            *sample = (sine as i32 * peak / 14336) as i16;
        }
    }
}

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
        for (group, cycles) in CYCLES.iter().enumerate() {
            let wave = &waves[group * DEPTH_COUNT + 4];
            for index in 0..WAVE_SAMPLE_COUNT {
                assert_eq!(wave[index], SINE[(index * cycles) % WAVE_SAMPLE_COUNT]);
            }
        }
        for wave in 0..WAVE_COUNT {
            assert_eq!(encoded[wave * 32 + 1], 4);
            assert_eq!(encoded[wave * 32 + 17], 3);
        }
    }
}
