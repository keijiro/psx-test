use crate::waveform::{wave_index, SHAPE_COUNT, WAVE_SAMPLE_COUNT};

// Quarter units allow noninteger ratios without floating point.
const RATIO_QUARTERS: i32 = 4;
const RATIO_COUNT: i32 = 20;

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
    note: i32,
    ratio: i32,
    depth: i32,
    mod_shape: i32,
    carrier_shape: i32,
    carrier_attack_ms: i32,
    carrier_release_ms: i32,
    mod_attack_ms: i32,
    mod_release_ms: i32,
    selected: i32,
}

#[repr(C)]
pub struct EnvelopeProgram {
    duration_ms: i32,
    carrier_pitch: u16,
    mod_pitch: u16,
    carrier_adsr1: u16,
    carrier_adsr2: u16,
    mod_adsr1: u16,
    mod_adsr2: u16,
    mod_wave: u16,
    carrier_wave: u16,
    frequency: u16,
}

#[repr(C)]
pub struct AdjustmentRepeat {
    held: u16,
    frames: i32,
}

impl Default for SynthSettings {
    fn default() -> Self {
        Self {
            note: 60,
            ratio: 3,
            depth: 6,
            mod_shape: 0,
            carrier_shape: 0,
            carrier_attack_ms: 10,
            carrier_release_ms: 450,
            mod_attack_ms: 5,
            mod_release_ms: 180,
            selected: 0,
        }
    }
}

enum Setting {
    Note,
    Ratio,
    Depth,
    ModShape,
    CarrierShape,
    CarrierAttack,
    CarrierRelease,
    ModAttack,
    ModRelease,
}

impl Setting {
    fn from_index(index: i32) -> Option<Self> {
        Some(match index {
            0 => Self::Note,
            1 => Self::Ratio,
            2 => Self::Depth,
            3 => Self::ModShape,
            4 => Self::CarrierShape,
            5 => Self::CarrierAttack,
            6 => Self::CarrierRelease,
            7 => Self::ModAttack,
            8 => Self::ModRelease,
            _ => return None,
        })
    }
}

fn adjust_clamped(value: &mut i32, adjustment: i32, min: i32, max: i32) -> bool {
    let previous = *value;
    *value = value.saturating_add(adjustment).clamp(min, max);
    *value != previous
}

impl SynthSettings {
    pub(crate) fn select(&mut self, direction: i32) {
        self.selected = (self.selected + direction).rem_euclid(9);
    }

    pub(crate) fn adjust(&mut self, adjustment: i32) -> bool {
        match Setting::from_index(self.selected) {
            Some(Setting::Note) => adjust_clamped(&mut self.note, adjustment, 24, 72),
            Some(Setting::Ratio) => adjust_clamped(&mut self.ratio, adjustment, 0, RATIO_COUNT - 1),
            Some(Setting::Depth) => adjust_clamped(&mut self.depth, adjustment, 0, 10),
            Some(Setting::ModShape) => {
                adjust_clamped(&mut self.mod_shape, adjustment, 0, SHAPE_COUNT as i32 - 1)
            }
            Some(Setting::CarrierShape) => adjust_clamped(
                &mut self.carrier_shape,
                adjustment,
                0,
                SHAPE_COUNT as i32 - 1,
            ),
            Some(Setting::CarrierAttack) => {
                adjust_clamped(&mut self.carrier_attack_ms, adjustment, 0, 500)
            }
            Some(Setting::CarrierRelease) => {
                adjust_clamped(&mut self.carrier_release_ms, adjustment, 1, 500)
            }
            Some(Setting::ModAttack) => adjust_clamped(&mut self.mod_attack_ms, adjustment, 0, 500),
            Some(Setting::ModRelease) => {
                adjust_clamped(&mut self.mod_release_ms, adjustment, 1, 500)
            }
            None => false,
        }
    }

    pub(crate) fn build_program(&self) -> EnvelopeProgram {
        let carrier_attack = nearest_rate(&ATTACK_MS, self.carrier_attack_ms);
        let carrier_release = nearest_rate(&SUSTAIN_MS, self.carrier_release_ms);
        let mod_attack = nearest_rate(&ATTACK_MS, self.mod_attack_ms);
        let mod_release = nearest_rate(&SUSTAIN_MS, self.mod_release_ms);
        let carrier_pitch = note_pitch(self.note) as u16;
        let (mod_pitch, mod_wave) =
            modulator_pitch_and_wave(self.note, self.ratio, self.depth, self.mod_shape);
        EnvelopeProgram {
            duration_ms: (self.carrier_attack_ms + self.carrier_release_ms)
                .max(self.mod_attack_ms + self.mod_release_ms),
            carrier_pitch,
            mod_pitch,
            // Level 15 bypasses decay; decreasing sustain provides one-shot release.
            carrier_adsr1: ((carrier_attack << 8) | 0x00ff) as u16,
            carrier_adsr2: (0xc000 | (carrier_release << 6)) as u16,
            mod_adsr1: ((mod_attack << 8) | 0x00ff) as u16,
            mod_adsr2: (0xc000 | (mod_release << 6)) as u16,
            mod_wave,
            carrier_wave: wave_index(self.carrier_shape as usize, 4) as u16,
            frequency: (MIDI_MILLIHZ[(self.note - 24) as usize] / 1000) as u16,
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

fn modulator_pitch_and_wave(note: i32, ratio: i32, depth: i32, shape: i32) -> (u16, u16) {
    let pitch = note_pitch(note) * (ratio + 1) / RATIO_QUARTERS;
    let wave = wave_index(shape as usize, depth as usize);
    (pitch as u16, wave as u16)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{synth_build_program, synth_default_settings, synth_read_adjustment};

    #[test]
    fn default_program_uses_independent_operators() {
        let mut settings = SynthSettings::default();
        unsafe {
            synth_default_settings(&mut settings);
        }
        assert_eq!((settings.note, settings.ratio, settings.depth), (60, 3, 6));
        let mut program = EnvelopeProgram {
            duration_ms: 0,
            carrier_pitch: 0,
            mod_pitch: 0,
            carrier_adsr1: 0,
            carrier_adsr2: 0,
            mod_adsr1: 0,
            mod_adsr2: 0,
            mod_wave: 0,
            carrier_wave: 0,
            frequency: 0,
        };
        unsafe {
            synth_build_program(&settings, &mut program);
        }
        assert_eq!(program.duration_ms, 460);
        assert_eq!(program.carrier_pitch, program.mod_pitch);
        assert_eq!(program.mod_wave, 6);
        assert_eq!(program.carrier_wave, 4);
        assert_ne!(program.carrier_adsr2, program.mod_adsr2);
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
    fn ratios_fit_the_single_cycle_pitch_range() {
        let mut settings = SynthSettings::default();
        assert!(settings.adjust(100));
        assert_eq!(settings.note, 72);
        settings.select(1);
        assert!(settings.adjust(100));
        assert_eq!(settings.ratio, 19);
        let program = settings.build_program();
        assert_eq!(program.mod_wave, wave_index(0, 6) as u16);
        assert_eq!(program.mod_pitch as i32, program.carrier_pitch as i32 * 5);
        assert!(settings.adjust(-100));
        assert_eq!(settings.ratio, 0);
        assert_eq!(
            settings.build_program().mod_pitch,
            settings.build_program().carrier_pitch / 4
        );
        for note in 24..=72 {
            for ratio in 0..RATIO_COUNT {
                let (pitch, wave) = modulator_pitch_and_wave(note, ratio, 10, 0);
                let requested = note_pitch(note) * (ratio + 1) / RATIO_QUARTERS;
                assert!(pitch < 0x4000);
                assert!(wave < crate::waveform::WAVE_COUNT as u16);
                assert_eq!(pitch as i32, requested);
            }
        }
    }

    #[test]
    fn fractional_ratios_set_modulator_pitch() {
        let mut settings = SynthSettings::default();
        settings.select(1);
        assert!(settings.adjust(-1));
        let program = settings.build_program();
        assert_eq!(
            program.mod_pitch as i32,
            program.carrier_pitch as i32 * 3 / 4
        );
        assert!(settings.adjust(2));
        assert_eq!(settings.ratio, 4);
        let program = settings.build_program();
        assert_eq!(
            program.mod_pitch as i32,
            program.carrier_pitch as i32 * 5 / 4
        );
        assert!(settings.adjust(1));
        assert_eq!(
            settings.build_program().mod_pitch as i32,
            program.carrier_pitch as i32 * 3 / 2
        );
    }

    #[test]
    fn operators_select_shapes_independently() {
        let mut settings = SynthSettings::default();
        settings.select(3);
        assert!(settings.adjust(100));
        assert!(!settings.adjust(1));
        settings.select(1);
        assert!(settings.adjust(2));
        let program = settings.build_program();
        assert_eq!(program.mod_wave, wave_index(SHAPE_COUNT - 1, 6) as u16);
        assert_eq!(program.carrier_wave, wave_index(2, 4) as u16);
        assert_eq!(program.carrier_pitch, program.mod_pitch);
        for mod_shape in 0..SHAPE_COUNT {
            settings.mod_shape = mod_shape as i32;
            for carrier_shape in 0..SHAPE_COUNT {
                settings.carrier_shape = carrier_shape as i32;
                let program = settings.build_program();
                assert_eq!(program.mod_wave, wave_index(mod_shape, 6) as u16);
                assert_eq!(program.carrier_wave, wave_index(carrier_shape, 4) as u16);
            }
        }
        settings.carrier_shape = 0;
        assert!(settings.adjust(100));
        assert!(!settings.adjust(1));
        assert_eq!(settings.carrier_shape, SHAPE_COUNT as i32 - 1);
        assert!(settings.adjust(-100));
        assert!(!settings.adjust(-1));
        assert_eq!(settings.carrier_shape, 0);
        settings.select(-1);
        assert!(settings.adjust(-100));
        assert!(!settings.adjust(-1));
        assert_eq!(settings.mod_shape, 0);
    }

    #[test]
    fn c_struct_sizes_match_header() {
        assert_eq!(core::mem::size_of::<SynthSettings>(), 40);
        assert_eq!(core::mem::size_of::<EnvelopeProgram>(), 24);
        assert_eq!(core::mem::size_of::<AdjustmentRepeat>(), 8);
    }
}
