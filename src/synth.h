#ifndef SYNTH_H
#define SYNTH_H

#include <stdint.h>

#define WAVE_SAMPLE_COUNT 56
#define WAVE_BLOCK_COUNT 2
#define WAVE_DATA_SIZE (WAVE_BLOCK_COUNT * 16)
#define WAVE_DEPTH_COUNT 11
#define WAVE_COUNT (WAVE_DEPTH_COUNT * 3)
#define ENVELOPE_MAX_MS 500
#define NOTE_MIN 24
#define NOTE_MAX 72
#define SETTING_COUNT 7

enum { SYNTH_ADJUST_LEFT = 1, SYNTH_ADJUST_RIGHT = 2,
	SYNTH_ADJUST_FAST_LEFT = 4, SYNTH_ADJUST_FAST_RIGHT = 8 };

typedef struct {
	int32_t note, ratio, depth;
	int32_t carrier_attack_ms, carrier_release_ms;
	int32_t mod_attack_ms, mod_release_ms, selected;
} SynthSettings;

typedef struct {
	int32_t duration_ms;
	uint16_t carrier_pitch, mod_pitch;
	uint16_t carrier_adsr1, carrier_adsr2;
	uint16_t mod_adsr1, mod_adsr2;
	uint16_t mod_wave, frequency;
} EnvelopeProgram;

typedef struct {
	uint16_t held;
	int32_t frames;
} AdjustmentRepeat;

_Static_assert(sizeof(SynthSettings) == 32, "SynthSettings ABI mismatch");
_Static_assert(sizeof(EnvelopeProgram) == 20, "EnvelopeProgram ABI mismatch");
_Static_assert(sizeof(AdjustmentRepeat) == 8, "AdjustmentRepeat ABI mismatch");

// The caller provides WAVE_COUNT * WAVE_SAMPLE_COUNT samples and
// WAVE_COUNT * WAVE_DATA_SIZE output bytes.
void synth_build_waves(int16_t *samples, uint8_t *adpcm);
void synth_default_settings(SynthSettings *settings);
void synth_select_setting(SynthSettings *settings, int32_t direction);
int32_t synth_adjust_setting(SynthSettings *settings, int32_t adjustment);
int32_t synth_read_adjustment(AdjustmentRepeat *repeat, uint16_t held);
void synth_build_program(const SynthSettings *settings, EnvelopeProgram *program);

#endif
