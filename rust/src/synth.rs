use crate::waveform::WAVE_SAMPLE_COUNT;

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

impl Default for SynthSettings {
    fn default() -> Self {
        Self {
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
        }
    }
}

enum Setting {
    WaveA,
    WaveB,
    Note,
    AmplitudeAttack,
    AmplitudeRelease,
    MixAttack,
    MixRelease,
    PitchSweep,
    PitchCurve,
}

impl Setting {
    fn from_index(index: i32) -> Option<Self> {
        Some(match index {
            0 => Self::WaveA,
            1 => Self::WaveB,
            2 => Self::Note,
            3 => Self::AmplitudeAttack,
            4 => Self::AmplitudeRelease,
            5 => Self::MixAttack,
            6 => Self::MixRelease,
            7 => Self::PitchSweep,
            8 => Self::PitchCurve,
            _ => return None,
        })
    }
}

fn adjust_clamped(value: &mut i32, adjustment: i32, min: i32, max: i32) -> bool {
    let previous = *value;
    *value = value.saturating_add(adjustment).clamp(min, max);
    *value != previous
}

fn rotate_wave(value: &mut i32, adjustment: i32) -> bool {
    let previous = *value;
    let direction = if adjustment < 0 { -1 } else { 1 };
    *value = (*value + direction).rem_euclid(crate::waveform::WAVE_COUNT as i32);
    *value != previous
}

impl SynthSettings {
    pub(crate) fn select(&mut self, direction: i32) {
        self.selected = (self.selected + direction).rem_euclid(9);
    }

    pub(crate) fn adjust(&mut self, adjustment: i32) -> bool {
        match Setting::from_index(self.selected) {
            Some(Setting::WaveA) => rotate_wave(&mut self.wave_a, adjustment),
            Some(Setting::WaveB) => rotate_wave(&mut self.wave_b, adjustment),
            Some(Setting::Note) => adjust_clamped(&mut self.note, adjustment, 24, 96),
            Some(Setting::AmplitudeAttack) => {
                adjust_clamped(&mut self.amplitude_attack_ms, adjustment, 0, 500)
            }
            Some(Setting::AmplitudeRelease) => {
                adjust_clamped(&mut self.amplitude_release_ms, adjustment, 1, 500)
            }
            Some(Setting::MixAttack) => adjust_clamped(&mut self.mix_attack_ms, adjustment, 0, 500),
            Some(Setting::MixRelease) => {
                adjust_clamped(&mut self.mix_release_ms, adjustment, 0, 500)
            }
            Some(Setting::PitchSweep) => adjust_clamped(&mut self.pitch_sweep, adjustment, -24, 24),
            Some(Setting::PitchCurve) => adjust_clamped(&mut self.pitch_curve, adjustment, 1, 16),
            None => false,
        }
    }

    pub(crate) fn build_program(&self) -> EnvelopeProgram {
        let attack = nearest_rate(&ATTACK_MS, self.amplitude_attack_ms);
        let release = nearest_rate(&SUSTAIN_MS, self.amplitude_release_ms);
        // Level 15 bypasses decay; decreasing sustain provides one-shot release.
        EnvelopeProgram {
            duration_ms: (self.amplitude_attack_ms + self.amplitude_release_ms).clamp(1, 1000),
            mix_attack_ms: self.mix_attack_ms,
            mix_release_ms: self.mix_release_ms,
            pitch_start: note_pitch((self.note + self.pitch_sweep).clamp(24, 96)),
            pitch_end: note_pitch(self.note),
            pitch_curve: self.pitch_curve,
            adsr1: ((attack << 8) | 0x00ff) as u16,
            adsr2: (0xc000 | (release << 6)) as u16,
        }
    }
}

impl AdjustmentRepeat {
    pub(crate) fn read(&mut self, held: u16) -> i32 {
        const LEFT: u16 = 1;
        const RIGHT: u16 = 2;
        const L1: u16 = 4;
        const R1: u16 = 8;
        if held == 0 {
            self.held = 0;
            return 0;
        }
        if held != self.held {
            self.held = held;
            self.frames = 18;
        } else if self.frames > 0 {
            self.frames -= 1;
            return 0;
        } else {
            self.frames = 2;
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

impl EnvelopeProgram {
    pub(crate) fn evaluate_mix(&self, elapsed: i32) -> i32 {
        if elapsed < self.mix_attack_ms {
            return elapsed * 256 / self.mix_attack_ms;
        }
        let release_elapsed = elapsed - self.mix_attack_ms;
        if self.mix_release_ms == 0 || release_elapsed >= self.mix_release_ms {
            return 0;
        }
        256 - release_elapsed * 256 / self.mix_release_ms
    }

    pub(crate) fn evaluate_pitch(&self, elapsed: i32) -> u16 {
        let progress = (elapsed * 256 / self.duration_ms).clamp(0, 256);
        let remaining = 256 - progress;
        let mut shaped = remaining;
        for _ in 1..self.pitch_curve {
            shaped = shaped * remaining / 256;
        }
        (self.pitch_end + (self.pitch_start - self.pitch_end) * shaped / 256) as u16
    }
}

pub(crate) fn midi_frequency_millihz(note: i32) -> i32 {
    MIDI_MILLIHZ[(note - 24) as usize]
}

#[cfg(test)]
mod tests {
    use crate::*;

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
    fn setting_adjustment_keeps_wave_wrap_and_numeric_limits() {
        let mut settings = SynthSettings::default();
        assert!(settings.adjust(-1));
        assert_eq!(settings.wave_a, 4);
        settings.select(2);
        assert!(settings.adjust(100));
        assert_eq!(settings.note, 96);
        assert!(!settings.adjust(1));
        settings.select(-3);
        assert_eq!(settings.selected, 8);
        assert!(settings.adjust(-100));
        assert_eq!(settings.pitch_curve, 1);
    }

    #[test]
    fn c_struct_sizes_match_header() {
        assert_eq!(core::mem::size_of::<SynthSettings>(), 40);
        assert_eq!(core::mem::size_of::<EnvelopeProgram>(), 28);
        assert_eq!(core::mem::size_of::<AdjustmentRepeat>(), 8);
    }
}
