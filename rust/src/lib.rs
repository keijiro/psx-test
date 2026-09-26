#![no_std]

mod adpcm;
mod synth;
mod waveform;

pub use synth::{AdjustmentRepeat, EnvelopeProgram, SynthSettings};

#[cfg(not(test))]
use core::panic::PanicInfo;

#[cfg(not(test))]
#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}

fn rounded(value: i32, divisor: i32) -> i32 {
    if value >= 0 {
        (value + divisor / 2) / divisor
    } else {
        -((-value + divisor / 2) / divisor)
    }
}

// The C caller supplies WAVE_COUNT * WAVE_SAMPLE_COUNT samples and
// WAVE_COUNT * 32 ADPCM bytes, with disjoint writable storage.
#[no_mangle]
pub unsafe extern "C" fn synth_build_waves(samples: *mut i16, adpcm: *mut u8) {
    let waves = &mut *(samples as *mut [[i16; waveform::WAVE_SAMPLE_COUNT]; waveform::WAVE_COUNT]);
    let encoded = &mut *(adpcm as *mut [[u8; 32]; waveform::WAVE_COUNT]);
    waveform::build_waves(waves, encoded);
}

#[no_mangle]
pub unsafe extern "C" fn synth_default_settings(settings: *mut SynthSettings) {
    *settings = SynthSettings::default();
}

#[no_mangle]
pub unsafe extern "C" fn synth_select_setting(settings: *mut SynthSettings, direction: i32) {
    (*settings).select(direction);
}

#[no_mangle]
pub unsafe extern "C" fn synth_adjust_setting(
    settings: *mut SynthSettings,
    adjustment: i32,
) -> i32 {
    i32::from((*settings).adjust(adjustment))
}

#[no_mangle]
pub unsafe extern "C" fn synth_read_adjustment(repeat: *mut AdjustmentRepeat, held: u16) -> i32 {
    (*repeat).read(held)
}

#[no_mangle]
pub unsafe extern "C" fn synth_build_program(
    settings: *const SynthSettings,
    program: *mut EnvelopeProgram,
) {
    *program = (*settings).build_program();
}

#[no_mangle]
pub unsafe extern "C" fn synth_evaluate_mix(program: *const EnvelopeProgram, elapsed: i32) -> i32 {
    (*program).evaluate_mix(elapsed)
}

#[no_mangle]
pub unsafe extern "C" fn synth_evaluate_pitch(
    program: *const EnvelopeProgram,
    elapsed: i32,
) -> u16 {
    (*program).evaluate_pitch(elapsed)
}

#[no_mangle]
pub extern "C" fn synth_midi_frequency_millihz(note: i32) -> i32 {
    synth::midi_frequency_millihz(note)
}
