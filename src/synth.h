#ifndef SYNTH_H
#define SYNTH_H

#include <stdint.h>

#define WAVE_SAMPLE_COUNT 56
#define WAVE_BLOCK_COUNT 2
#define WAVE_DATA_SIZE (WAVE_BLOCK_COUNT * 16)
#define WAVE_COUNT 5
#define ENVELOPE_MAX_MS 500
#define NOTE_MIN 24
#define NOTE_MAX 96
#define SETTING_COUNT 9

enum { SYNTH_ADJUST_LEFT = 1, SYNTH_ADJUST_RIGHT = 2,
	SYNTH_ADJUST_FAST_LEFT = 4, SYNTH_ADJUST_FAST_RIGHT = 8 };

typedef struct {
	int32_t wave_a, wave_b, note;
	int32_t amplitude_attack_ms, amplitude_release_ms;
	int32_t mix_attack_ms, mix_release_ms;
	int32_t pitch_sweep, pitch_curve, selected;
} SynthSettings;

typedef struct {
	int32_t duration_ms, mix_attack_ms, mix_release_ms;
	int32_t pitch_start, pitch_end, pitch_curve;
	uint16_t adsr1, adsr2;
} EnvelopeProgram;

typedef struct {
	uint16_t held;
	int32_t frames;
} AdjustmentRepeat;

_Static_assert(sizeof(SynthSettings) == 40, "SynthSettings ABI mismatch");
_Static_assert(sizeof(EnvelopeProgram) == 28, "EnvelopeProgram ABI mismatch");
_Static_assert(sizeof(AdjustmentRepeat) == 8, "AdjustmentRepeat ABI mismatch");

// The caller provides WAVE_COUNT * WAVE_SAMPLE_COUNT samples and
// WAVE_COUNT * WAVE_DATA_SIZE output bytes.
void synth_build_waves(int16_t *samples, uint8_t *adpcm);
void synth_default_settings(SynthSettings *settings);
void synth_select_setting(SynthSettings *settings, int32_t direction);
int32_t synth_adjust_setting(SynthSettings *settings, int32_t adjustment);
int32_t synth_read_adjustment(AdjustmentRepeat *repeat, uint16_t held);
void synth_build_program(const SynthSettings *settings, EnvelopeProgram *program);
int32_t synth_evaluate_mix(const EnvelopeProgram *program, int32_t elapsed_ms);
uint16_t synth_evaluate_pitch(const EnvelopeProgram *program, int32_t elapsed_ms);
int32_t synth_midi_frequency_millihz(int32_t note);

#endif
