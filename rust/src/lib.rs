#![no_std]

#[cfg(not(test))]
use core::panic::PanicInfo;

#[cfg(not(test))]
#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}

const WAVE_SAMPLE_COUNT: usize = 56;
const WAVE_COUNT: usize = 5;
const WAVE_SAMPLE_PEAK: i32 = 14336;
const BLOCK_SAMPLES: usize = 28;
const FILTERS: [(i32, i32); 5] = [(0, 0), (60, 0), (115, -52), (98, -55), (122, -60)];
const SINE: [i16; WAVE_SAMPLE_COUNT] = [
    0, 1605, 3190, 4735, 6220, 7627, 8938, 10137, 11208, 12139, 12916, 13532, 13977, 14246, 14336,
    14246, 13977, 13532, 12916, 12139, 11208, 10137, 8938, 7627, 6220, 4735, 3190, 1605, 0, -1605,
    -3190, -4735, -6220, -7627, -8938, -10137, -11208, -12139, -12916, -13532, -13977, -14246,
    -14336, -14246, -13977, -13532, -12916, -12139, -11208, -10137, -8938, -7627, -6220, -4735,
    -3190, -1605,
];

// These rounded durations come from the SPU's 44.1 kHz ADSR generator.
const ATTACK_MS: [u16; 55] = [
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 2, 2, 2, 3, 3, 4, 5, 6, 7, 8, 9,
    12, 13, 15, 19, 23, 27, 31, 37, 46, 53, 62, 74, 93, 106, 124, 149, 186, 212, 248, 297, 372,
    425, 495, 594,
];
const SUSTAIN_MS: [u16; 49] = [
    0, 0, 0, 1, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 4, 4, 5, 6, 7, 8, 10, 11, 13, 15, 18, 20, 23, 26, 31,
    35, 40, 46, 55, 61, 69, 79, 94, 104, 117, 134, 157, 173, 192, 218, 252, 275, 303, 339, 505,
];
// The table avoids floating point and 64-bit division in the timer callback.
const MIDI_MILLIHZ: [i32; 73] = [
    32703, 34648, 36708, 38891, 41203, 43654, 46249, 48999, 51913, 55000, 58270, 61735, 65406,
    69296, 73416, 77782, 82407, 87307, 92499, 97999, 103826, 110000, 116541, 123471, 130813,
    138591, 146832, 155563, 164814, 174614, 184997, 195998, 207652, 220000, 233082, 246942, 261626,
    277183, 293665, 311127, 329628, 349228, 369994, 391995, 415305, 440000, 466164, 493883, 523251,
    554365, 587330, 622254, 659255, 698456, 739989, 783991, 830609, 880000, 932328, 987767,
    1046502, 1108731, 1174659, 1244508, 1318510, 1396913, 1479978, 1567982, 1661219, 1760000,
    1864655, 1975533, 2093005,
];

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SynthSettings {
    wave_a: i32,
    wave_b: i32,
    note: i32,
    amplitude_attack_ms: i32,
    amplitude_release_ms: i32,
    mix_attack_ms: i32,
    mix_release_ms: i32,
    pitch_sweep: i32,
    pitch_curve: i32,
    selected: i32,
}

#[repr(C)]
pub struct EnvelopeProgram {
    duration_ms: i32,
    mix_attack_ms: i32,
    mix_release_ms: i32,
    pitch_start: i32,
    pitch_end: i32,
    pitch_curve: i32,
    adsr1: u16,
    adsr2: u16,
}

#[repr(C)]
pub struct AdjustmentRepeat {
    held: u16,
    frames: i32,
}

fn rounded(value: i32, divisor: i32) -> i32 {
    if value >= 0 {
        (value + divisor / 2) / divisor
    } else {
        -((-value + divisor / 2) / divisor)
    }
}

#[derive(Clone, Copy, Default)]
struct History {
    previous_1: i32,
    previous_2: i32,
}

fn predict(history: History, filter: usize) -> i32 {
    let (a, b) = FILTERS[filter];
    (history.previous_1 * a + history.previous_2 * b + 32) >> 6
}

fn decode(nibble: i32, shift: usize, filter: usize, history: &mut History) -> i32 {
    let sample = (predict(*history, filter) + nibble * (1 << (12 - shift))).clamp(-32768, 32767);
    history.previous_2 = history.previous_1;
    history.previous_1 = sample;
    sample
}

fn encode_block(dest: &mut [u8], samples: &[i16], flags: u8, history: &mut History) {
    let mut best_error = u64::MAX;
    let mut best_filter = 0;
    let mut best_shift = 0;
    let mut best_nibbles = [0i8; BLOCK_SAMPLES];
    // Compare reconstructed error so predictor feedback affects the choice.
    for filter in 0..FILTERS.len() {
        for shift in 0..=12 {
            let mut candidate = *history;
            let mut error_sum = 0u64;
            let step = 1 << (12 - shift);
            let mut nibbles = [0i8; BLOCK_SAMPLES];
            for index in 0..BLOCK_SAMPLES {
                let nibble =
                    rounded(samples[index] as i32 - predict(candidate, filter), step).clamp(-8, 7);
                let error = decode(nibble, shift, filter, &mut candidate) - samples[index] as i32;
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
    for index in (0..BLOCK_SAMPLES).step_by(2) {
        decode(best_nibbles[index] as i32, best_shift, best_filter, history);
        decode(
            best_nibbles[index + 1] as i32,
            best_shift,
            best_filter,
            history,
        );
        dest[2 + index / 2] =
            (best_nibbles[index] as u8 & 15) | ((best_nibbles[index + 1] as u8) << 4);
    }
}

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
    let mean = rounded(sum, WAVE_SAMPLE_COUNT as i32);
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

#[no_mangle]
pub unsafe extern "C" fn synth_build_waves(samples: *mut i16, adpcm: *mut u8) {
    let waves = &mut *(samples as *mut [[i16; WAVE_SAMPLE_COUNT]; WAVE_COUNT]);
    let encoded = core::slice::from_raw_parts_mut(adpcm, WAVE_COUNT * 32);
    build_samples(waves);
    for wave in 0..WAVE_COUNT {
        let mut history = History::default();
        // Carry predictor state across passes to converge on the loop boundary.
        for _ in 0..8 {
            for block in 0..2 {
                let flags = if block == 0 { 0x04 } else { 0x03 };
                encode_block(
                    &mut encoded[wave * 32 + block * 16..][..16],
                    &waves[wave][block * BLOCK_SAMPLES..][..BLOCK_SAMPLES],
                    flags,
                    &mut history,
                );
            }
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn synth_default_settings(settings: *mut SynthSettings) {
    *settings = SynthSettings {
        wave_a: 0,
        wave_b: 2,
        note: 60,
        amplitude_attack_ms: 20,
        amplitude_release_ms: 400,
        mix_attack_ms: 120,
        mix_release_ms: 280,
        pitch_sweep: 12,
        pitch_curve: 3,
        selected: 0,
    };
}

#[no_mangle]
pub unsafe extern "C" fn synth_select_setting(settings: *mut SynthSettings, direction: i32) {
    let settings = &mut *settings;
    settings.selected = (settings.selected + direction + 9) % 9;
}

#[no_mangle]
pub unsafe extern "C" fn synth_adjust_setting(
    settings: *mut SynthSettings,
    adjustment: i32,
) -> i32 {
    let settings = &mut *settings;
    let direction = if adjustment < 0 { -1 } else { 1 };
    let value = match settings.selected {
        0 => &mut settings.wave_a,
        1 => &mut settings.wave_b,
        2 => &mut settings.note,
        3 => &mut settings.amplitude_attack_ms,
        4 => &mut settings.amplitude_release_ms,
        5 => &mut settings.mix_attack_ms,
        6 => &mut settings.mix_release_ms,
        7 => &mut settings.pitch_sweep,
        8 => &mut settings.pitch_curve,
        _ => return 0,
    };
    let previous = *value;
    *value = match settings.selected {
        0 | 1 => (*value + direction + WAVE_COUNT as i32) % WAVE_COUNT as i32,
        2 => (*value + adjustment).clamp(24, 96),
        3 | 5 | 6 => (*value + adjustment).clamp(0, 500),
        4 => (*value + adjustment).clamp(1, 500),
        7 => (*value + adjustment).clamp(-24, 24),
        _ => (*value + adjustment).clamp(1, 16),
    };
    i32::from(*value != previous)
}

#[no_mangle]
pub unsafe extern "C" fn synth_read_adjustment(repeat: *mut AdjustmentRepeat, held: u16) -> i32 {
    const LEFT: u16 = 1;
    const RIGHT: u16 = 2;
    const L1: u16 = 4;
    const R1: u16 = 8;
    let repeat = &mut *repeat;
    if held == 0 {
        repeat.held = 0;
        return 0;
    }
    if held != repeat.held {
        repeat.held = held;
        repeat.frames = 18;
    } else if repeat.frames > 0 {
        repeat.frames -= 1;
        return 0;
    } else {
        repeat.frames = 2;
    }
    // Shoulder buttons override a held D-pad direction without a release.
    if held & (L1 | R1) != 0 {
        if held & (L1 | R1) == L1 | R1 {
            return 0;
        }
        return if held & L1 != 0 { -10 } else { 10 };
    }
    if held & (LEFT | RIGHT) == LEFT | RIGHT {
        return 0;
    }
    if held & LEFT != 0 {
        -1
    } else {
        1
    }
}

fn nearest_rate(durations: &[u16], target: i32) -> i32 {
    let mut nearest = 0;
    let mut error = target;
    for (index, duration) in durations.iter().enumerate() {
        let difference = (*duration as i32 - target).abs();
        if difference < error {
            nearest = index as i32;
            error = difference;
        }
    }
    nearest
}

fn note_pitch(note: i32) -> i32 {
    let sample_rate = (MIDI_MILLIHZ[(note - 24) as usize] * WAVE_SAMPLE_COUNT as i32 + 500) / 1000;
    sample_rate * 4096 / 44100
}

#[no_mangle]
pub unsafe extern "C" fn synth_build_program(
    settings: *const SynthSettings,
    program: *mut EnvelopeProgram,
) {
    let settings = &*settings;
    let attack = nearest_rate(&ATTACK_MS, settings.amplitude_attack_ms);
    let release = nearest_rate(&SUSTAIN_MS, settings.amplitude_release_ms);
    // Level 15 bypasses decay; decreasing sustain provides one-shot release.
    *program = EnvelopeProgram {
        duration_ms: (settings.amplitude_attack_ms + settings.amplitude_release_ms).clamp(1, 1000),
        mix_attack_ms: settings.mix_attack_ms,
        mix_release_ms: settings.mix_release_ms,
        pitch_start: note_pitch((settings.note + settings.pitch_sweep).clamp(24, 96)),
        pitch_end: note_pitch(settings.note),
        pitch_curve: settings.pitch_curve,
        adsr1: ((attack << 8) | 0x00ff) as u16,
        adsr2: (0xc000 | (release << 6)) as u16,
    };
}

#[no_mangle]
pub unsafe extern "C" fn synth_evaluate_mix(program: *const EnvelopeProgram, elapsed: i32) -> i32 {
    let program = &*program;
    if elapsed < program.mix_attack_ms {
        return elapsed * 256 / program.mix_attack_ms;
    }
    let release_elapsed = elapsed - program.mix_attack_ms;
    if program.mix_release_ms == 0 || release_elapsed >= program.mix_release_ms {
        return 0;
    }
    256 - release_elapsed * 256 / program.mix_release_ms
}

#[no_mangle]
pub unsafe extern "C" fn synth_evaluate_pitch(
    program: *const EnvelopeProgram,
    elapsed: i32,
) -> u16 {
    let program = &*program;
    let progress = (elapsed * 256 / program.duration_ms).clamp(0, 256);
    let remaining = 256 - progress;
    let mut shaped = remaining;
    for _ in 1..program.pitch_curve {
        shaped = shaped * remaining / 256;
    }
    (program.pitch_end + (program.pitch_start - program.pitch_end) * shaped / 256) as u16
}

#[no_mangle]
pub extern "C" fn synth_midi_frequency_millihz(note: i32) -> i32 {
    MIDI_MILLIHZ[(note - 24) as usize]
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_and_envelope_match_demo() {
        let mut settings = SynthSettings {
            wave_a: 0,
            wave_b: 0,
            note: 0,
            amplitude_attack_ms: 0,
            amplitude_release_ms: 0,
            mix_attack_ms: 0,
            mix_release_ms: 0,
            pitch_sweep: 0,
            pitch_curve: 0,
            selected: 0,
        };
        unsafe {
            synth_default_settings(&mut settings);
        }
        assert_eq!(
            (settings.wave_a, settings.wave_b, settings.note),
            (0, 2, 60)
        );
        let mut program = EnvelopeProgram {
            duration_ms: 0,
            mix_attack_ms: 0,
            mix_release_ms: 0,
            pitch_start: 0,
            pitch_end: 0,
            pitch_curve: 0,
            adsr1: 0,
            adsr2: 0,
        };
        unsafe {
            synth_build_program(&settings, &mut program);
        }
        assert_eq!(program.duration_ms, 420);
        assert_eq!(unsafe { synth_evaluate_mix(&program, 0) }, 0);
        assert_eq!(unsafe { synth_evaluate_mix(&program, 120) }, 256);
        assert_eq!(unsafe { synth_evaluate_mix(&program, 400) }, 0);
        assert_eq!(
            unsafe { synth_evaluate_pitch(&program, 420) } as i32,
            program.pitch_end
        );
    }

    #[test]
    fn adjustment_repeats_after_the_initial_delay() {
        let mut repeat = AdjustmentRepeat { held: 0, frames: 0 };
        assert_eq!(unsafe { synth_read_adjustment(&mut repeat, 1) }, -1);
        for _ in 0..18 {
            assert_eq!(unsafe { synth_read_adjustment(&mut repeat, 1) }, 0);
        }
        assert_eq!(unsafe { synth_read_adjustment(&mut repeat, 1) }, -1);
        assert_eq!(unsafe { synth_read_adjustment(&mut repeat, 1 | 8) }, 10);
        assert_eq!(unsafe { synth_read_adjustment(&mut repeat, 0) }, 0);
    }

    #[test]
    fn generated_adpcm_has_loop_flags() {
        let mut waves = [[0i16; WAVE_SAMPLE_COUNT]; WAVE_COUNT];
        let mut encoded = [0u8; WAVE_COUNT * 32];
        unsafe {
            synth_build_waves(waves.as_mut_ptr() as *mut i16, encoded.as_mut_ptr());
        }
        assert_eq!(waves[0], SINE);
        for wave in 0..WAVE_COUNT {
            assert_eq!(encoded[wave * 32 + 1], 4);
            assert_eq!(encoded[wave * 32 + 17], 3);
        }
    }
}
