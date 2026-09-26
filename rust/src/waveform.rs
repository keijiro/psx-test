pub(crate) const WAVE_SAMPLE_COUNT: usize = 56;
pub(crate) const WAVE_COUNT: usize = 5;
const WAVE_SAMPLE_PEAK: i32 = 14336;
pub(crate) const SINE: [i16; WAVE_SAMPLE_COUNT] = [
    0, 1605, 3190, 4735, 6220, 7627, 8938, 10137, 11208, 12139, 12916, 13532, 13977, 14246, 14336,
    14246, 13977, 13532, 12916, 12139, 11208, 10137, 8938, 7627, 6220, 4735, 3190, 1605, 0, -1605,
    -3190, -4735, -6220, -7627, -8938, -10137, -11208, -12139, -12916, -13532, -13977, -14246,
    -14336, -14246, -13977, -13532, -12916, -12139, -11208, -10137, -8938, -7627, -6220, -4735,
    -3190, -1605,
];

fn build_samples(waves: &mut [[i16; WAVE_SAMPLE_COUNT]; WAVE_COUNT]) {
    for index in 0..WAVE_SAMPLE_COUNT {
        waves[0][index] = SINE[index];
        let phase = (index + WAVE_SAMPLE_COUNT / 4) % WAVE_SAMPLE_COUNT;
        waves[1][index] = if phase < WAVE_SAMPLE_COUNT / 2 {
            -WAVE_SAMPLE_PEAK + phase as i32 * 4 * WAVE_SAMPLE_PEAK / WAVE_SAMPLE_COUNT as i32
        } else {
            3 * WAVE_SAMPLE_PEAK - phase as i32 * 4 * WAVE_SAMPLE_PEAK / WAVE_SAMPLE_COUNT as i32
        } as i16;
        waves[2][index] = (-WAVE_SAMPLE_PEAK
            + index as i32 * 2 * WAVE_SAMPLE_PEAK / WAVE_SAMPLE_COUNT as i32)
            as i16;
        waves[3][index] = if index < WAVE_SAMPLE_COUNT / 2 {
            WAVE_SAMPLE_PEAK
        } else {
            -WAVE_SAMPLE_PEAK
        } as i16;
    }
    // Center and normalize a fixed cycle so the display and sound are deterministic.
    let mut noise = 0x6d2b79f5u32;
    let mut sum = 0;
    for sample in &mut waves[4] {
        noise ^= noise << 13;
        noise ^= noise >> 17;
        noise ^= noise << 5;
        *sample =
            (((noise & 0xffff) as i32 * (2 * WAVE_SAMPLE_PEAK) / 65535) - WAVE_SAMPLE_PEAK) as i16;
        sum += *sample as i32;
    }
    let mean = crate::rounded(sum, WAVE_SAMPLE_COUNT as i32);
    let mut peak = 1;
    for sample in &mut waves[4] {
        let centered = *sample as i32 - mean;
        peak = peak.max(centered.abs());
        *sample = centered as i16;
    }
    for sample in &mut waves[4] {
        *sample = (*sample as i32 * WAVE_SAMPLE_PEAK / peak) as i16;
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
        assert_eq!(waves[0], SINE);
        for wave in 0..WAVE_COUNT {
            assert_eq!(encoded[wave * 32 + 1], 4);
            assert_eq!(encoded[wave * 32 + 17], 3);
        }
        // This fingerprint records the ADPCM bytes produced before the refactor.
        let hash = encoded.iter().fold(0xcbf29ce484222325u64, |hash, &byte| {
            (hash ^ byte as u64).wrapping_mul(0x100000001b3)
        });
        assert_eq!(hash, 0x10e3530ba8bf83f1);
    }
}
