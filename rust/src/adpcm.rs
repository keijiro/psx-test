const BLOCK_SAMPLES: usize = 28;
const FILTERS: [(i32, i32); 5] = [(0, 0), (60, 0), (115, -52), (98, -55), (122, -60)];

#[derive(Clone, Copy, Default, PartialEq, Eq)]
pub(crate) struct AdpcmEncoder {
    previous_1: i32,
    previous_2: i32,
}

impl AdpcmEncoder {
    fn predict(&self, filter: usize) -> i32 {
        let (a, b) = FILTERS[filter];
        (self.previous_1 * a + self.previous_2 * b + 32) >> 6
    }

    fn decode(&mut self, nibble: i32, shift: usize, filter: usize) -> i32 {
        let sample = (self.predict(filter) + nibble * (1 << (12 - shift))).clamp(-32768, 32767);
        self.previous_2 = self.previous_1;
        self.previous_1 = sample;
        sample
    }

    pub(crate) fn encode_block(
        &mut self,
        dest: &mut [u8; 16],
        samples: &[i16; BLOCK_SAMPLES],
        flags: u8,
    ) {
        let mut best_error = u64::MAX;
        let mut best_filter = 0;
        let mut best_shift = 0;
        let mut best_nibbles = [0i8; BLOCK_SAMPLES];
        // Compare reconstructed error so predictor feedback affects the choice.
        for filter in 0..FILTERS.len() {
            for shift in 0..=12 {
                let mut candidate = *self;
                let mut error_sum = 0u64;
                let step = 1 << (12 - shift);
                let mut nibbles = [0i8; BLOCK_SAMPLES];
                for index in 0..BLOCK_SAMPLES {
                    let nibble =
                        crate::rounded(samples[index] as i32 - candidate.predict(filter), step)
                            .clamp(-8, 7);
                    let error = candidate.decode(nibble, shift, filter) - samples[index] as i32;
                    nibbles[index] = nibble as i8;
                    error_sum += (error as i64 * error as i64) as u64;
                }
                if error_sum < best_error {
                    best_error = error_sum;
                    best_filter = filter;
                    best_shift = shift;
                    best_nibbles = nibbles;
                }
            }
        }
        dest[0] = ((best_filter << 4) | best_shift) as u8;
        dest[1] = flags;
        for (pair, byte) in best_nibbles.chunks_exact(2).zip(dest[2..].iter_mut()) {
            self.decode(pair[0] as i32, best_shift, best_filter);
            self.decode(pair[1] as i32, best_shift, best_filter);
            *byte = (pair[0] as u8 & 15) | ((pair[1] as u8) << 4);
        }
    }
}

fn decode_loop(data: &[u8; 32], state: &mut AdpcmEncoder) -> [i16; 56] {
    let mut samples = [0i16; 56];
    for block in 0..2 {
        let shift = (data[block * 16] & 15) as usize;
        let filter = (data[block * 16] >> 4) as usize;
        for index in 0..28 {
            let byte = data[block * 16 + 2 + index / 2];
            let nibble = ((byte >> (4 * (index % 2))) & 15) as i32;
            let nibble = if nibble >= 8 { nibble - 16 } else { nibble };
            samples[block * 28 + index] = state.decode(nibble, shift, filter) as i16;
        }
    }
    samples
}

fn loop_metrics(data: &[u8; 32], samples: &[i16; 56]) -> Option<(i32, i32, u64)> {
    let mut state = AdpcmEncoder::default();
    // Bound host-side evaluation; candidates with longer transients or periods
    // are rejected in favor of the guaranteed balanced fallback.
    let mut histories = [AdpcmEncoder::default(); 128];
    for cycle in 0..histories.len() {
        if let Some(start) = histories[..cycle]
            .iter()
            .position(|&history| history == state)
        {
            // Integer predictor rounding can produce a multi-loop limit cycle
            // rather than a fixed history. Measure DC over the complete period.
            let period = (cycle - start) as i32;
            let mut sum = 0i32;
            let mut error = 0u64;
            for _ in 0..period {
                let decoded = decode_loop(data, &mut state);
                sum += decoded.iter().map(|&sample| sample as i32).sum::<i32>();
                error += squared_error(samples, &decoded);
            }
            return Some((sum, period, error / period as u64));
        }
        histories[cycle] = state;
        decode_loop(data, &mut state);
    }
    None
}

fn squared_error(samples: &[i16; 56], decoded: &[i16; 56]) -> u64 {
    samples
        .iter()
        .zip(decoded)
        .map(|(&target, &actual)| {
            let error = actual as i64 - target as i64;
            (error * error) as u64
        })
        .sum()
}

pub(crate) fn encode_loop(output: &mut [u8; 32], samples: &[i16; 56]) {
    let mut best_error = u64::MAX;
    // Predictor-free blocks provide an exact zero-DC fallback. Constrain the
    // sum of their nibbles to zero, choosing each adjustment by its distortion
    // cost. This also handles the asymmetric -8..7 range at maximum depth.
    for shift in 0..=12 {
        let step = 1 << (12 - shift);
        let mut nibbles = samples.map(|sample| crate::rounded(sample as i32, step).clamp(-8, 7));
        let mut sum: i32 = nibbles.iter().sum();
        while sum != 0 {
            let adjustment = -sum.signum();
            let index = (0..56)
                .filter(|&index| (-8..=7).contains(&(nibbles[index] + adjustment)))
                .min_by_key(|&index| {
                    let error = nibbles[index] as i64 * step as i64 - samples[index] as i64;
                    let next = error + adjustment as i64 * step as i64;
                    next * next - error * error
                })
                .unwrap();
            nibbles[index] += adjustment;
            sum += adjustment;
        }
        let mut candidate = [0u8; 32];
        for block in 0..2 {
            candidate[block * 16] = shift as u8;
            candidate[block * 16 + 1] = if block == 0 { 4 } else { 3 };
            for pair in 0..14 {
                let index = block * 28 + pair * 2;
                candidate[block * 16 + 2 + pair] =
                    (nibbles[index] as u8 & 15) | ((nibbles[index + 1] as u8) << 4);
            }
        }
        let (_, _, error) = loop_metrics(&candidate, samples).unwrap();
        if error < best_error {
            best_error = error;
            *output = candidate;
        }
    }

    // Retain predictive encoding when it is more accurate and its steady-loop
    // mean is within one PCM unit of zero (less than 0.053 cent at full ADSR).
    // Evaluate the final bytes, not the encoder's last pass: replaying a fixed
    // loop can settle to a different predictor history. Correct measured DC
    // in the input and retry; coarse quantization may oscillate, in which case
    // the balanced fallback above remains available.
    let mut bias = 0i32;
    let mut visited = [i32::MAX; 128];
    for attempt in 0..visited.len() {
        if visited[..attempt].contains(&bias) {
            // A one-unit input change can select a different predictor or
            // quantization path. Search nearby biases when feedback repeats.
            bias = (1..=128)
                .flat_map(|distance| [bias - distance, bias + distance])
                .find(|value| !visited[..attempt].contains(value))
                .unwrap();
        }
        visited[attempt] = bias;
        let adjusted = samples.map(|sample| (sample as i32 + bias).clamp(-32768, 32767) as i16);
        let mut candidate = [0u8; 32];
        let mut encoder = AdpcmEncoder::default();
        for _ in 0..8 {
            for block in 0..2 {
                encoder.encode_block(
                    (&mut candidate[block * 16..][..16]).try_into().unwrap(),
                    (&adjusted[block * 28..][..28]).try_into().unwrap(),
                    if block == 0 { 4 } else { 3 },
                );
            }
        }
        let Some((sum, period, error)) = loop_metrics(&candidate, samples) else {
            break;
        };
        if sum.abs() <= 56 * period {
            if error < best_error {
                *output = candidate;
            }
            break;
        }
        bias -= crate::rounded(sum, 56 * period);
    }
}
