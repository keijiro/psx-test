#[path = "src/adpcm.rs"]
mod adpcm;
#[path = "src/waveform_config.rs"]
mod waveform_config;
mod waveforms;

fn rounded(value: i32, divisor: i32) -> i32 {
    if value >= 0 {
        (value + divisor / 2) / divisor
    } else {
        -((-value + divisor / 2) / divisor)
    }
}

fn main() {
    let mut samples = [[0i16; waveform_config::WAVE_SAMPLE_COUNT]; waveform_config::WAVE_COUNT];
    let mut encoded = [[0u8; 32]; waveform_config::WAVE_COUNT];
    // Shape generation uses floating point and encoding searches many predictor
    // states. Prepare both banks on the host so startup only copies the loops.
    waveforms::build_waves(&mut samples, &mut encoded);
    let output = std::path::PathBuf::from(std::env::var_os("OUT_DIR").unwrap());
    let pcm: Vec<u8> = samples
        .into_iter()
        .flatten()
        .flat_map(i16::to_le_bytes)
        .collect();
    std::fs::write(output.join("waves.pcm"), pcm).unwrap();
    let bytes: Vec<u8> = encoded.into_iter().flatten().collect();
    std::fs::write(output.join("waves.adpcm"), bytes).unwrap();
    println!("cargo:rerun-if-changed=waveforms.rs");
    println!("cargo:rerun-if-changed=src/waveform_config.rs");
    println!("cargo:rerun-if-changed=src/adpcm.rs");
}
