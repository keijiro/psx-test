pub(crate) use crate::waveform_config::{DEPTH_COUNT, SHAPE_COUNT, WAVE_COUNT, WAVE_SAMPLE_COUNT};

const PCM_WAVES: &[u8; WAVE_COUNT * WAVE_SAMPLE_COUNT * 2] =
    include_bytes!(concat!(env!("OUT_DIR"), "/waves.pcm"));

pub(crate) fn wave_index(shape: usize, depth: usize) -> usize {
    shape * DEPTH_COUNT + depth
}

pub(crate) fn build_samples(waves: &mut [[i16; WAVE_SAMPLE_COUNT]; WAVE_COUNT]) {
    // Explicit little-endian reads avoid requiring alignment from include_bytes!
    // and keep the host tests identical to the MIPS bank copied at startup.
    for (sample, bytes) in waves.iter_mut().flatten().zip(PCM_WAVES.chunks_exact(2)) {
        *sample = i16::from_le_bytes([bytes[0], bytes[1]]);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn decode_loop(data: &[u8], history: &mut [i32; 2]) -> [i32; WAVE_SAMPLE_COUNT] {
        // Decode independently from the encoder, retaining history through
        // actual loop jumps rather than trusting its own error estimate.
        let coefficients = [(0, 0), (60, 0), (115, -52), (98, -55), (122, -60)];
        let mut decoded = [0i32; WAVE_SAMPLE_COUNT];
        for block in 0..2 {
            let header = data[block * 16];
            let (a, b) = coefficients[(header >> 4) as usize];
            for sample in 0..28 {
                let byte = data[block * 16 + 2 + sample / 2];
                let nibble = ((byte >> (sample % 2 * 4)) & 15) as i32;
                let residual =
                    (if nibble < 8 { nibble } else { nibble - 16 }) * (4096 >> (header & 15));
                let value =
                    (residual + ((a * history[0] + b * history[1] + 32) >> 6)).clamp(-32768, 32767);
                *history = [value, history[0]];
                decoded[block * 28 + sample] = value;
            }
        }
        decoded
    }

    #[test]
    fn generated_adpcm_matches_build_output() {
        let mut waves = [[0i16; WAVE_SAMPLE_COUNT]; WAVE_COUNT];
        let mut encoded = [0u8; WAVE_COUNT * 32];
        unsafe {
            crate::synth_build_waves(waves.as_mut_ptr() as *mut i16, encoded.as_mut_ptr());
        }
        assert!(waves[0].iter().all(|&sample| sample == 0));
        assert_eq!(waves[4], crate::waveforms::SINE);
        assert_eq!(waves[8][14], 28672);
        assert_eq!(waves[10][14], 32766);
        for shape in 0..SHAPE_COUNT {
            assert!(waves[wave_index(shape, 0)]
                .iter()
                .all(|&sample| sample == 0));
        }
        for wave in 0..WAVE_COUNT {
            assert_eq!(encoded[wave * 32 + 1], 4);
            assert_eq!(encoded[wave * 32 + 17], 3);
        }
        let mut expected_samples = [[0i16; WAVE_SAMPLE_COUNT]; WAVE_COUNT];
        let mut expected_adpcm = [[0u8; 32]; WAVE_COUNT];
        crate::waveforms::build_waves(&mut expected_samples, &mut expected_adpcm);
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
            assert!(
                wave.iter()
                    .map(|&sample| (sample as i32).abs())
                    .max()
                    .unwrap()
                    >= peak - 1
            );
        }
    }

    #[test]
    fn all_shapes_survive_encoding_as_distinct_loops() {
        let mut waves = [[0i16; WAVE_SAMPLE_COUNT]; WAVE_COUNT];
        build_samples(&mut waves);
        let mut decoded = [[0i32; WAVE_SAMPLE_COUNT]; SHAPE_COUNT];
        for shape in 0..SHAPE_COUNT {
            let index = wave_index(shape, 4);
            let data = &crate::ENCODED_WAVES[index * 32..][..32];
            let mut history = [0i32; 2];
            for _ in 0..128 {
                decoded[shape] = decode_loop(data, &mut history);
            }
            for previous in 0..shape {
                let previous_index = wave_index(previous, 4);
                assert_ne!(
                    waves[index], waves[previous_index],
                    "source shapes {previous}/{shape}"
                );
                assert_ne!(
                    decoded[shape], decoded[previous],
                    "decoded shapes {previous}/{shape}"
                );
            }
        }
    }

    #[test]
    fn fixed_adpcm_loops_keep_pitch_centered() {
        let mut waves = [[0i16; WAVE_SAMPLE_COUNT]; WAVE_COUNT];
        build_samples(&mut waves);
        for (index, data) in crate::ENCODED_WAVES.chunks_exact(32).enumerate() {
            let mut history = [0i32; 2];
            for _ in 0..128 {
                decode_loop(data, &mut history);
            }
            // Some predictors settle into periods of several loops. A fixed
            // averaging window can cut that period short and mismeasure DC.
            // After the encoder's bounded transient, measure a complete period.
            let start = history;
            let mut total = 0i64;
            let mut error = 0i64;
            let mut period = 0;
            for cycle in 1..=128 {
                let decoded = decode_loop(data, &mut history);
                total += decoded.iter().map(|&sample| sample as i64).sum::<i64>();
                error += decoded
                    .iter()
                    .zip(waves[index])
                    .map(|(&actual, target)| {
                        let difference = actual as i64 - target as i64;
                        difference * difference
                    })
                    .sum::<i64>();
                if index % DEPTH_COUNT == 0 {
                    assert!(decoded.iter().all(|&sample| sample == 0));
                }
                if history == start {
                    period = cycle;
                    break;
                }
            }
            assert!(
                period > 0,
                "decoded wave {index} has no bounded steady period"
            );
            assert!(
                total.abs() <= (period * WAVE_SAMPLE_COUNT) as i64,
                "decoded wave {index}: sum {total}, period {period}"
            );
            // The corrected bank's worst RMS reconstruction errors are 0.41%
            // for sine and 1.63% for triangle. Limits of 1% and 2% guard against
            // losing predictor quality when the DC search changes.
            if index / DEPTH_COUNT == 0 || index / DEPTH_COUNT == 1 {
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
                    error * limit <= energy * period as i64,
                    "waveform error at wave {index}: {error}/{energy}"
                );
            }
        }
    }
}
