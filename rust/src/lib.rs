#![no_std]

#[cfg(not(target_arch = "mips"))]
mod adpcm;
mod synth;
mod waveform;

const ENCODED_WAVES: &[u8; waveform::WAVE_COUNT * 32] =
    include_bytes!(concat!(env!("OUT_DIR"), "/waves.adpcm"));

pub use synth::{AdjustmentRepeat, EnvelopeProgram, SynthSettings};

#[cfg(not(test))]
use core::panic::PanicInfo;

#[cfg(not(test))]
#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}

#[cfg(not(target_arch = "mips"))]
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
    let encoded = &mut *(adpcm as *mut [u8; waveform::WAVE_COUNT * 32]);
    waveform::build_samples(waves);
    encoded.copy_from_slice(ENCODED_WAVES);
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
