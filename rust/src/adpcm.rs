const BLOCK_SAMPLES: usize = 28;
const FILTERS: [(i32, i32); 5] = [(0, 0), (60, 0), (115, -52), (98, -55), (122, -60)];

#[derive(Clone, Copy, Default)]
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
