#[path = "src/adpcm.rs"]
mod adpcm;
#[allow(dead_code)]
#[path = "src/waveform.rs"]
mod waveform;

fn rounded(value: i32, divisor: i32) -> i32 {
    if value >= 0 {
        (value + divisor / 2) / divisor
    } else {
        -((-value + divisor / 2) / divisor)
    }
}

fn main() {
    let mut samples = [[0i16; waveform::WAVE_SAMPLE_COUNT]; waveform::WAVE_COUNT];
    let mut encoded = [[0u8; 32]; waveform::WAVE_COUNT];
    // Encoding searches many predictor states; do it on the build host so
    // the PS1 only copies the finished loops into SPU RAM at startup.
    waveform::build_waves(&mut samples, &mut encoded);
    let bytes: Vec<u8> = encoded.into_iter().flatten().collect();
    let path = std::path::Path::new(&std::env::var("OUT_DIR").unwrap()).join("waves.adpcm");
    std::fs::write(path, bytes).unwrap();
    println!("cargo:rerun-if-changed=src/waveform.rs");
    println!("cargo:rerun-if-changed=src/adpcm.rs");
}
