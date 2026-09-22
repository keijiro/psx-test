#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include <psxapi.h>
#include <psxetc.h>
#include <psxgpu.h>
#include <psxpad.h>
#include <psxspu.h>

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define OT_LENGTH 16
#define PACKET_BUFFER_LENGTH 8192
#define WAVE_SAMPLE_COUNT 56
#define ADPCM_BLOCK_SAMPLE_COUNT 28
#define WAVE_BLOCK_COUNT 2
#define WAVE_DATA_SIZE (WAVE_BLOCK_COUNT * 16)
#define WAVE_COUNT 5
#define ADPCM_FILTER_COUNT 5
#define ADPCM_ENCODING_PASSES 8
#define WAVE_SAMPLE_PEAK 14336
#define WAVE_DATA_ADDR 0x1010
#define WAVE_A_CHANNEL 0
#define WAVE_B_CHANNEL 1
#define VOICE_MASK ((1 << WAVE_A_CHANNEL) | (1 << WAVE_B_CHANNEL))
#define VOICE_VOLUME 0x3000
#define ADSR_MAX_LEVEL 0x7fff
#define ENVELOPE_TICK_RATE 1000
#define ENVELOPE_MAX_MS 500
#define ADJUST_FAST_MULTIPLIER 10
// Input is sampled once per VSync: this gives an approximately 300 ms pause,
// then a 20 Hz repeat rate on the NTSC display used by the demo.
#define ADJUST_REPEAT_DELAY_FRAMES 18
#define ADJUST_REPEAT_INTERVAL_FRAMES 3
#define NOTE_MIN 24
#define NOTE_MAX 96
#define SWEEP_MIN -24
#define SWEEP_MAX 24
#define CURVE_MIN 1
#define CURVE_MAX 16
#define SETTING_COUNT 9

enum { WAVE_SINE, WAVE_TRIANGLE, WAVE_SAW, WAVE_SQUARE, WAVE_NOISE };

typedef struct {
	DISPENV disp_env;
	DRAWENV draw_env;
	uint32_t ordering_table[OT_LENGTH];
	uint8_t packet_buffer[PACKET_BUFFER_LENGTH];
} RenderBuffer;

typedef struct {
	RenderBuffer buffers[2];
	uint8_t *next_packet;
	int active_buffer;
} RenderContext;

typedef struct {
	int envelope_tick;
	int mix;
	int amplitude;
	int frequency;
} Sequencer;

typedef struct {
	int wave_a;
	int wave_b;
	int note;
	int amplitude_attack_ms;
	int amplitude_release_ms;
	int mix_attack_ms;
	int mix_release_ms;
	int pitch_sweep;
	int pitch_curve;
	int selected;
} SynthSettings;

typedef struct {
	int duration_ms;
	int mix_attack_ms;
	int mix_release_ms;
	int pitch_start;
	int pitch_end;
	int pitch_curve;
	uint16_t adsr1;
	uint16_t adsr2;
} EnvelopeProgram;

typedef struct { int previous_1; int previous_2; } AdpcmHistory;

static const int adpcm_filter_coefficients[ADPCM_FILTER_COUNT][2] = {
	{ 0, 0 }, { 60, 0 }, { 115, -52 }, { 98, -55 }, { 122, -60 }
};
static const char *const wave_names[WAVE_COUNT] = {
	"SINE", "TRIANGLE", "SAW", "SQUARE", "NOISE"
};
static const int16_t sine_samples[WAVE_SAMPLE_COUNT] = {
	0, 1605, 3190, 4735, 6220, 7627, 8938,
	10137, 11208, 12139, 12916, 13532, 13977, 14246,
	14336, 14246, 13977, 13532, 12916, 12139, 11208,
	10137, 8938, 7627, 6220, 4735, 3190, 1605,
	0, -1605, -3190, -4735, -6220, -7627, -8938,
	-10137, -11208, -12139, -12916, -13532, -13977, -14246,
	-14336, -14246, -13977, -13532, -12916, -12139, -11208,
	-10137, -8938, -7627, -6220, -4735, -3190, -1605
};

// These are the rounded durations produced by the SPU's 44.1 kHz ADSR
// generator. The closest entries make the millisecond controls useful while
// leaving the actual amplitude envelope entirely on the SPU.
static const uint16_t attack_rate_ms[] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	1, 1, 1, 1, 1, 1, 2, 2, 2, 3, 3, 4, 5, 6,
	7, 8, 9, 12, 13, 15, 19, 23, 27, 31, 37, 46,
	53, 62, 74, 93, 106, 124, 149, 186, 212, 248, 297, 372, 425, 495, 594
};
static const uint16_t sustain_rate_ms[] = {
	0, 0, 0, 1, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 4, 4, 5,
	6, 7, 8, 10, 11, 13, 15, 18, 20, 23, 26, 31, 35, 40, 46,
	55, 61, 69, 79, 94, 104, 117, 134, 157, 173, 192, 218,
	252, 275, 303, 339, 505
};

static const uint32_t midi_frequency_table[] = {
	32703, 34648, 36708, 38891, 41203, 43654, 46249, 48999, 51913, 55000,
	58270, 61735, 65406, 69296, 73416, 77782, 82407, 87307, 92499, 97999,
	103826, 110000, 116541, 123471, 130813, 138591, 146832, 155563, 164814,
	174614, 184997, 195998, 207652, 220000, 233082, 246942, 261626, 277183,
	293665, 311127, 329628, 349228, 369994, 391995, 415305, 440000, 466164,
	493883, 523251, 554365, 587330, 622254, 659255, 698456, 739989, 783991,
	830609, 880000, 932328, 987767, 1046502, 1108731, 1174659, 1244508,
	1318510, 1396913, 1479978, 1567982, 1661219, 1760000, 1864655,
	1975533, 2093005
};

static RenderContext render_context;
static volatile Sequencer sequencer = { 0, 0, 0, 0 };
static SynthSettings synth_settings = {
	WAVE_SINE, WAVE_SAW, 60, 20, 400, 120, 280, 12, 3, 0
};
static EnvelopeProgram envelope_programs[2];
static volatile int active_envelope_program;
static volatile int playback_active;
static int software_envelope_started;
static volatile int trigger_requested;
static uint8_t pad_buffers[2][34];
static uint16_t repeating_adjustment_buttons;
static int adjustment_repeat_frames;
static int16_t wave_samples[WAVE_COUNT][WAVE_SAMPLE_COUNT];
static uint32_t wave_data[(WAVE_DATA_SIZE * WAVE_COUNT) / sizeof(uint32_t)];

static int clamp(int value, int minimum, int maximum) {
	if (value < minimum) return minimum;
	if (value > maximum) return maximum;
	return value;
}

static int divide_rounded(int value, int divisor) {
	if (value >= 0) return (value + divisor / 2) / divisor;
	return -((-value + divisor / 2) / divisor);
}

static int predict_adpcm_sample(const AdpcmHistory *history, int filter) {
	return (history->previous_1 * adpcm_filter_coefficients[filter][0] +
		history->previous_2 * adpcm_filter_coefficients[filter][1] + 32) >> 6;
}

static int decode_adpcm_sample(
	int nibble, int shift, int filter, AdpcmHistory *history
) {
	int sample = predict_adpcm_sample(history, filter) +
		nibble * (1 << (12 - shift));
	sample = clamp(sample, -32768, 32767);
	history->previous_2 = history->previous_1;
	history->previous_1 = sample;
	return sample;
}

static void encode_adpcm_block(
	uint8_t *destination, const int16_t *samples, uint8_t flags,
	AdpcmHistory *history
) {
	uint64_t best_error = UINT64_MAX;
	int best_filter = 0;
	int best_shift = 0;
	int8_t best_nibbles[ADPCM_BLOCK_SAMPLE_COUNT];

	// Choose parameters using reconstructed error so predictor feedback is part
	// of the decision rather than hidden behind the input quantization error.
	for (int filter = 0; filter < ADPCM_FILTER_COUNT; filter++) {
		for (int shift = 0; shift <= 12; shift++) {
			AdpcmHistory candidate_history = *history;
			uint64_t error_sum = 0;
			int step = 1 << (12 - shift);
			int8_t nibbles[ADPCM_BLOCK_SAMPLE_COUNT];
			for (int index = 0; index < ADPCM_BLOCK_SAMPLE_COUNT; index++) {
				int prediction = predict_adpcm_sample(&candidate_history, filter);
				int nibble = clamp(
					divide_rounded(samples[index] - prediction, step), -8, 7
				);
				int decoded = decode_adpcm_sample(
					nibble, shift, filter, &candidate_history
				);
				int error = decoded - samples[index];
				nibbles[index] = nibble;
				error_sum += (int64_t) error * error;
			}
			if (error_sum < best_error) {
				best_error = error_sum;
				best_filter = filter;
				best_shift = shift;
				for (int index = 0; index < ADPCM_BLOCK_SAMPLE_COUNT; index++)
					best_nibbles[index] = nibbles[index];
			}
		}
	}

	destination[0] = (best_filter << 4) | best_shift;
	destination[1] = flags;
	for (int index = 0; index < ADPCM_BLOCK_SAMPLE_COUNT; index += 2) {
		decode_adpcm_sample(best_nibbles[index], best_shift, best_filter, history);
		decode_adpcm_sample(
			best_nibbles[index + 1], best_shift, best_filter, history
		);
		destination[2 + index / 2] =
			((uint8_t) best_nibbles[index] & 0x0f) |
			((uint8_t) best_nibbles[index + 1] << 4);
	}
}

static void encode_wavetable(uint8_t *destination, const int16_t *samples) {
	AdpcmHistory history = { 0, 0 };
	// Repeated encoding carries predictor state through the loop boundary and
	// converges on sustained playback rather than optimizing only the first pass.
	for (int pass = 0; pass < ADPCM_ENCODING_PASSES; pass++) {
		for (int block = 0; block < WAVE_BLOCK_COUNT; block++) {
			uint8_t flags = (block == 0) ? 0x04 : 0x03;
			encode_adpcm_block(
				&destination[block * 16],
				&samples[block * ADPCM_BLOCK_SAMPLE_COUNT], flags, &history
			);
		}
	}
}

static void build_wave_samples(void) {
	for (int index = 0; index < WAVE_SAMPLE_COUNT; index++) {
		wave_samples[WAVE_SINE][index] = sine_samples[index];
		int phase = (index + WAVE_SAMPLE_COUNT / 4) % WAVE_SAMPLE_COUNT;
		wave_samples[WAVE_TRIANGLE][index] = phase < WAVE_SAMPLE_COUNT / 2 ?
			-WAVE_SAMPLE_PEAK + phase * 4 * WAVE_SAMPLE_PEAK / WAVE_SAMPLE_COUNT :
			3 * WAVE_SAMPLE_PEAK - phase * 4 * WAVE_SAMPLE_PEAK / WAVE_SAMPLE_COUNT;
		wave_samples[WAVE_SAW][index] =
			-WAVE_SAMPLE_PEAK + index * 2 * WAVE_SAMPLE_PEAK / WAVE_SAMPLE_COUNT;
		wave_samples[WAVE_SQUARE][index] =
			index < WAVE_SAMPLE_COUNT / 2 ? WAVE_SAMPLE_PEAK : -WAVE_SAMPLE_PEAK;
	}

	// A fixed cycle makes the noise and display deterministic. Centering and
	// normalizing it prevents a short random sequence from adding a DC offset.
	uint32_t noise = 0x6d2b79f5;
	int noise_sum = 0;
	for (int index = 0; index < WAVE_SAMPLE_COUNT; index++) {
		noise ^= noise << 13;
		noise ^= noise >> 17;
		noise ^= noise << 5;
		wave_samples[WAVE_NOISE][index] =
			(int32_t) (noise & 0xffff) * (2 * WAVE_SAMPLE_PEAK) / 65535 -
			WAVE_SAMPLE_PEAK;
		noise_sum += wave_samples[WAVE_NOISE][index];
	}
	int noise_mean = divide_rounded(noise_sum, WAVE_SAMPLE_COUNT);
	int noise_peak = 1;
	for (int index = 0; index < WAVE_SAMPLE_COUNT; index++) {
		int centered = wave_samples[WAVE_NOISE][index] - noise_mean;
		int magnitude = centered < 0 ? -centered : centered;
		if (magnitude > noise_peak) noise_peak = magnitude;
		wave_samples[WAVE_NOISE][index] = centered;
	}
	for (int index = 0; index < WAVE_SAMPLE_COUNT; index++) {
		wave_samples[WAVE_NOISE][index] =
			wave_samples[WAVE_NOISE][index] * WAVE_SAMPLE_PEAK / noise_peak;
	}
}

static int find_nearest_rate(
	const uint16_t *durations, int duration_count, int target_ms
) {
	int nearest = 0;
	int nearest_error = target_ms;
	for (int rate = 0; rate < duration_count; rate++) {
		int error = durations[rate] - target_ms;
		if (error < 0) error = -error;
		if (error < nearest_error) {
			nearest = rate;
			nearest_error = error;
		}
	}
	return nearest;
}

static uint16_t make_adsr1(int attack_rate) {
	// Sustain level 15 skips decay, leaving attack followed by sustain release.
	return (attack_rate << 8) | 0x00ff;
}

static uint16_t make_adsr2(int sustain_rate) {
	// Exponentially decreasing sustain supplies the one-shot release stage.
	return 0xc000 | (sustain_rate << 6);
}

static int midi_frequency_millihz(int note) {
	// Precomputed equal-tempered values avoid 64-bit division in the timer ISR
	// and make every selectable MIDI note deterministic on the R3000 CPU.
	return midi_frequency_table[note - NOTE_MIN];
}

static uint16_t note_pitch(int note) {
	int sample_rate =
		(midi_frequency_millihz(note) * WAVE_SAMPLE_COUNT + 500) / 1000;
	return getSPUSampleRate(sample_rate);
}

static EnvelopeProgram build_envelope_program(void) {
	EnvelopeProgram program;
	int attack_rate = find_nearest_rate(
		attack_rate_ms, sizeof(attack_rate_ms) / sizeof(attack_rate_ms[0]),
		synth_settings.amplitude_attack_ms
	);
	int release_rate = find_nearest_rate(
		sustain_rate_ms, sizeof(sustain_rate_ms) / sizeof(sustain_rate_ms[0]),
		synth_settings.amplitude_release_ms
	);
	program.duration_ms = clamp(
		synth_settings.amplitude_attack_ms + synth_settings.amplitude_release_ms,
		1, ENVELOPE_MAX_MS * 2
	);
	program.mix_attack_ms = synth_settings.mix_attack_ms;
	program.mix_release_ms = synth_settings.mix_release_ms;
	program.pitch_start = note_pitch(clamp(
		synth_settings.note + synth_settings.pitch_sweep, NOTE_MIN, NOTE_MAX
	));
	program.pitch_end = note_pitch(synth_settings.note);
	program.pitch_curve = synth_settings.pitch_curve;
	program.adsr1 = make_adsr1(attack_rate);
	program.adsr2 = make_adsr2(release_rate);
	return program;
}

static void rebuild_envelope(void) {
	int next_program = active_envelope_program ^ 1;
	envelope_programs[next_program] = build_envelope_program();
	FastEnterCriticalSection();
	active_envelope_program = next_program;
	FastExitCriticalSection();
}

static int evaluate_mix(const EnvelopeProgram *program, int elapsed_ms) {
	if (elapsed_ms < program->mix_attack_ms)
		return program->mix_attack_ms == 0 ?
			256 : elapsed_ms * 256 / program->mix_attack_ms;
	int release_elapsed = elapsed_ms - program->mix_attack_ms;
	if (program->mix_release_ms == 0 || release_elapsed >= program->mix_release_ms)
		return 0;
	return 256 - release_elapsed * 256 / program->mix_release_ms;
}

static uint16_t evaluate_pitch(const EnvelopeProgram *program, int elapsed_ms) {
	int progress = clamp(elapsed_ms * 256 / program->duration_ms, 0, 256);
	int remaining = 256 - progress;
	int shaped = remaining;
	for (int power = 1; power < program->pitch_curve; power++)
		shaped = shaped * remaining / 256;
	return program->pitch_end +
		(program->pitch_start - program->pitch_end) * shaped / 256;
}

static void set_voice_volume(int channel, int volume) {
	SPU_CH_VOL_L(channel) = volume;
	SPU_CH_VOL_R(channel) = volume;
}

static void setup_sound(void) {
	build_wave_samples();
	for (int wave = 0; wave < WAVE_COUNT; wave++)
		encode_wavetable(
			(uint8_t *) wave_data + wave * WAVE_DATA_SIZE, wave_samples[wave]
		);
	SpuInit();
	SpuSetTransferMode(SPU_TRANSFER_BY_DMA);
	SpuSetTransferStartAddr(WAVE_DATA_ADDR);
	SpuWrite(wave_data, sizeof(wave_data));
	SpuIsTransferCompleted(SPU_TRANSFER_WAIT);
}

static void apply_envelope_tick(int tick) {
	const EnvelopeProgram *program = &envelope_programs[active_envelope_program];
	int mix = evaluate_mix(program, tick);
	uint16_t pitch = evaluate_pitch(program, tick);
	set_voice_volume(WAVE_A_CHANNEL, VOICE_VOLUME * (256 - mix) / 256);
	set_voice_volume(WAVE_B_CHANNEL, VOICE_VOLUME * mix / 256);
	SPU_CH_FREQ(WAVE_A_CHANNEL) = pitch;
	SPU_CH_FREQ(WAVE_B_CHANNEL) = pitch;
	sequencer.mix = mix;
	sequencer.frequency = midi_frequency_millihz(synth_settings.note) / 1000;
}

static int sample_spu_envelope(void) {
	int level = SPU_CH_ADSR_VOL(WAVE_A_CHANNEL) & ADSR_MAX_LEVEL;
	sequencer.amplitude = level * 256 / ADSR_MAX_LEVEL;
	return level;
}

static void start_playback(void) {
	SpuSetKey(0, VOICE_MASK);
	const EnvelopeProgram *program = &envelope_programs[active_envelope_program];
	int wave_a_addr = WAVE_DATA_ADDR + synth_settings.wave_a * WAVE_DATA_SIZE;
	int wave_b_addr = WAVE_DATA_ADDR + synth_settings.wave_b * WAVE_DATA_SIZE;
	SPU_CH_ADDR(WAVE_A_CHANNEL) = getSPUAddr(wave_a_addr);
	SPU_CH_LOOP_ADDR(WAVE_A_CHANNEL) = getSPUAddr(wave_a_addr);
	SPU_CH_ADDR(WAVE_B_CHANNEL) = getSPUAddr(wave_b_addr);
	SPU_CH_LOOP_ADDR(WAVE_B_CHANNEL) = getSPUAddr(wave_b_addr);
	for (int channel = WAVE_A_CHANNEL; channel <= WAVE_B_CHANNEL; channel++) {
		SPU_CH_ADSR1(channel) = program->adsr1;
		SPU_CH_ADSR2(channel) = program->adsr2;
	}
	playback_active = 1;
	software_envelope_started = 0;
	sequencer.envelope_tick = 0;
	sequencer.amplitude = 0;
	apply_envelope_tick(0);
	SpuSetKey(1, VOICE_MASK);
}

static void timer_tick(void) {
	if (trigger_requested) {
		trigger_requested = 0;
		start_playback();
		return;
	}
	if (!playback_active) return;
	// PCSX-Redux defers key-on until its audio mixer runs. Keep the initial mix
	// and pitch until the SPU produces a real attack step so short Wave B
	// transients do not depend on the mixer phase relative to this timer.
	if (!software_envelope_started) {
		if (sample_spu_envelope() <= 1) return;
		software_envelope_started = 1;
		return;
	}
	const EnvelopeProgram *program = &envelope_programs[active_envelope_program];
	sequencer.envelope_tick++;
	if (sequencer.envelope_tick >= program->duration_ms) {
		SpuSetKey(0, VOICE_MASK);
		playback_active = 0;
		sequencer.amplitude = 0;
		return;
	}
	apply_envelope_tick(sequencer.envelope_tick);
	sample_spu_envelope();
}

static void setup_envelope_timer(void) {
	EnterCriticalSection();
	ChangeClearRCnt(2, 0);
	InterruptCallback(IRQ_TIMER2, &timer_tick);
	TIMER_RELOAD(2) = (F_CPU / 8) / ENVELOPE_TICK_RATE;
	TIMER_CTRL(2) = 0x0258; // CLK/8 input, repeated IRQ on target
	ExitCriticalSection();
}

static int adjust_setting(int adjustment) {
	int direction = adjustment < 0 ? -1 : 1;
	int previous;
	switch (synth_settings.selected) {
		case 0:
			previous = synth_settings.wave_a;
			synth_settings.wave_a =
				(synth_settings.wave_a + direction + WAVE_COUNT) % WAVE_COUNT;
			return synth_settings.wave_a != previous;
		case 1:
			previous = synth_settings.wave_b;
			synth_settings.wave_b =
				(synth_settings.wave_b + direction + WAVE_COUNT) % WAVE_COUNT;
			return synth_settings.wave_b != previous;
		case 2:
			previous = synth_settings.note;
			synth_settings.note = clamp(
				synth_settings.note + adjustment, NOTE_MIN, NOTE_MAX
			);
			return synth_settings.note != previous;
		case 3:
			previous = synth_settings.amplitude_attack_ms;
			synth_settings.amplitude_attack_ms = clamp(
				synth_settings.amplitude_attack_ms + adjustment,
				0, ENVELOPE_MAX_MS
			);
			return synth_settings.amplitude_attack_ms != previous;
		case 4:
			previous = synth_settings.amplitude_release_ms;
			synth_settings.amplitude_release_ms = clamp(
				synth_settings.amplitude_release_ms + adjustment,
				1, ENVELOPE_MAX_MS
			);
			return synth_settings.amplitude_release_ms != previous;
		case 5:
			previous = synth_settings.mix_attack_ms;
			synth_settings.mix_attack_ms = clamp(
				synth_settings.mix_attack_ms + adjustment,
				0, ENVELOPE_MAX_MS
			);
			return synth_settings.mix_attack_ms != previous;
		case 6:
			previous = synth_settings.mix_release_ms;
			synth_settings.mix_release_ms = clamp(
				synth_settings.mix_release_ms + adjustment,
				0, ENVELOPE_MAX_MS
			);
			return synth_settings.mix_release_ms != previous;
		case 7:
			previous = synth_settings.pitch_sweep;
			synth_settings.pitch_sweep = clamp(
				synth_settings.pitch_sweep + adjustment, SWEEP_MIN, SWEEP_MAX
			);
			return synth_settings.pitch_sweep != previous;
		case 8:
			previous = synth_settings.pitch_curve;
			synth_settings.pitch_curve = clamp(
				synth_settings.pitch_curve + adjustment, CURVE_MIN, CURVE_MAX
			);
			return synth_settings.pitch_curve != previous;
	}
	return 0;
}

static int read_adjustment(uint16_t buttons) {
	const uint16_t adjustment_mask = PAD_LEFT | PAD_RIGHT | PAD_L1 | PAD_R1;
	uint16_t held = (uint16_t) ~buttons & adjustment_mask;
	if (held == 0) {
		repeating_adjustment_buttons = 0;
		return 0;
	}

	int repeat = held == repeating_adjustment_buttons;
	if (!repeat) {
		repeating_adjustment_buttons = held;
		adjustment_repeat_frames = ADJUST_REPEAT_DELAY_FRAMES;
	} else if (adjustment_repeat_frames > 0) {
		adjustment_repeat_frames--;
		return 0;
	} else {
		adjustment_repeat_frames = ADJUST_REPEAT_INTERVAL_FRAMES - 1;
	}

	// Shoulder buttons take priority so a held D-pad direction can be
	// temporarily accelerated without first releasing it.
	if (held & (PAD_L1 | PAD_R1)) {
		if ((held & (PAD_L1 | PAD_R1)) == (PAD_L1 | PAD_R1)) return 0;
		return (held & PAD_L1) ?
			-ADJUST_FAST_MULTIPLIER : ADJUST_FAST_MULTIPLIER;
	}
	if ((held & (PAD_LEFT | PAD_RIGHT)) == (PAD_LEFT | PAD_RIGHT)) return 0;
	return (held & PAD_LEFT) ? -1 : 1;
}

static void setup_rendering(RenderContext *context) {
	ResetGraph(0);
	FntLoad(960, 0);
	SetDefDrawEnv(&context->buffers[0].draw_env, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
	SetDefDispEnv(&context->buffers[0].disp_env, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
	SetDefDrawEnv(&context->buffers[1].draw_env, 0, SCREEN_HEIGHT, SCREEN_WIDTH, SCREEN_HEIGHT);
	SetDefDispEnv(&context->buffers[1].disp_env, 0, SCREEN_HEIGHT, SCREEN_WIDTH, SCREEN_HEIGHT);
	for (int index = 0; index < 2; index++) {
		setRGB0(&context->buffers[index].draw_env, 7, 12, 24);
		context->buffers[index].draw_env.isbg = 1;
	}
	context->active_buffer = 0;
	context->next_packet = context->buffers[0].packet_buffer;
	ClearOTagR(context->buffers[0].ordering_table, OT_LENGTH);
	SetDispMask(1);
}

static void flip_buffers(RenderContext *context) {
	DrawSync(0);
	VSync(0);
	RenderBuffer *draw_buffer = &context->buffers[context->active_buffer];
	RenderBuffer *display_buffer = &context->buffers[context->active_buffer ^ 1];
	PutDispEnv(&display_buffer->disp_env);
	DrawOTagEnv(&draw_buffer->ordering_table[OT_LENGTH - 1], &draw_buffer->draw_env);
	context->active_buffer ^= 1;
	context->next_packet = display_buffer->packet_buffer;
	ClearOTagR(display_buffer->ordering_table, OT_LENGTH);
}

static void *allocate_primitive(RenderContext *context, int depth, size_t size) {
	RenderBuffer *buffer = &context->buffers[context->active_buffer];
	uint8_t *primitive = context->next_packet;
	addPrim(&buffer->ordering_table[depth], primitive);
	context->next_packet += size;
	assert(context->next_packet <= &buffer->packet_buffer[PACKET_BUFFER_LENGTH]);
	return primitive;
}

static void draw_text(RenderContext *context, int x, int y, const char *text) {
	RenderBuffer *buffer = &context->buffers[context->active_buffer];
	context->next_packet = (uint8_t *) FntSort(
		&buffer->ordering_table[0], context->next_packet, x, y, text
	);
	assert(context->next_packet <= &buffer->packet_buffer[PACKET_BUFFER_LENGTH]);
}

static char *append_text(char *destination, const char *source) {
	while (*source) *destination++ = *source++;
	return destination;
}

static char *append_number(char *destination, int value) {
	if (value < 0) {
		*destination++ = '-';
		value = -value;
	}
	int divisor = 1000;
	while (divisor > 1 && value < divisor) divisor /= 10;
	while (divisor > 0) {
		*destination++ = '0' + value / divisor;
		value %= divisor;
		divisor /= 10;
	}
	return destination;
}

static void draw_setting_text(
	RenderContext *context, int y, int index, const char *label,
	const char *value, const char *unit
) {
	char text[40];
	char *next = text;
	*next++ = synth_settings.selected == index ? '>' : ' ';
	*next++ = ' ';
	next = append_text(next, label);
	*next++ = ':';
	*next++ = ' ';
	next = append_text(next, value);
	if (*unit) {
		*next++ = ' ';
		next = append_text(next, unit);
	}
	*next = '\0';
	draw_text(context, 8, y, text);
}

static void draw_setting_number(
	RenderContext *context, int y, int index, const char *label, int value,
	const char *unit
) {
	char value_text[8];
	char *end = append_number(value_text, value);
	*end = '\0';
	draw_setting_text(context, y, index, label, value_text, unit);
}

static void draw_tile(
	RenderContext *context, int depth, int x, int y, int width, int height,
	int red, int green, int blue
) {
	TILE *tile = (TILE *) allocate_primitive(context, depth, sizeof(TILE));
	setTile(tile);
	setXY0(tile, x, y);
	setWH(tile, width, height);
	setRGB0(tile, red, green, blue);
}

static void draw_meters(RenderContext *context, const Sequencer *state) {
	const int x = 54;
	const int width = 212;
	draw_tile(context, 3, x, 143, width, 5, 28, 34, 48);
	draw_tile(context, 2, x, 143, width * state->mix / 256, 5, 255, 116, 48);
	draw_text(context, 8, 140, "MIX");
	draw_tile(context, 3, x, 154, width, 5, 28, 34, 48);
	draw_tile(context, 2, x, 154, width * state->amplitude / 256, 5, 88, 224, 128);
	draw_text(context, 8, 151, "AMP");
}

static void draw_waveform(RenderContext *context, const Sequencer *state) {
	const int graph_left = 19;
	const int graph_width = 282;
	const int center_y = 194;
	const int16_t *wave_a = wave_samples[synth_settings.wave_a];
	const int16_t *wave_b = wave_samples[synth_settings.wave_b];
	draw_tile(context, 3, graph_left, center_y, graph_width, 1, 38, 48, 66);
	for (int index = 0; index < WAVE_SAMPLE_COUNT - 1; index++) {
		int sample_a =
			(wave_a[index] * (256 - state->mix) + wave_b[index] * state->mix) / 256;
		int sample_b = (wave_a[index + 1] * (256 - state->mix) +
			wave_b[index + 1] * state->mix) / 256;
		LINE_F2 *line = (LINE_F2 *) allocate_primitive(context, 2, sizeof(LINE_F2));
		setLineF2(line);
		setXY2(
			line,
			graph_left + index * graph_width / (WAVE_SAMPLE_COUNT - 1),
			center_y - sample_a * 25 / WAVE_SAMPLE_PEAK,
			graph_left + (index + 1) * graph_width / (WAVE_SAMPLE_COUNT - 1),
			center_y - sample_b * 25 / WAVE_SAMPLE_PEAK
		);
		setRGB0(line, 60 + 195 * state->mix / 256, 150,
			255 - 207 * state->mix / 256);
	}
}

int main(void) {
	setup_rendering(&render_context);
	setup_sound();
	envelope_programs[0] = build_envelope_program();
	InitPAD(pad_buffers[0], sizeof(pad_buffers[0]), pad_buffers[1], sizeof(pad_buffers[1]));
	StartPAD();
	uint16_t previous_buttons = 0xffff;
	setup_envelope_timer();

	for (;;) {
		PADTYPE *pad = (PADTYPE *) pad_buffers[0];
		uint16_t buttons = pad->stat == 0 ? pad->btn : 0xffff;
		uint16_t pressed = previous_buttons & ~buttons;
		if (pressed & PAD_UP)
			synth_settings.selected =
				(synth_settings.selected + SETTING_COUNT - 1) % SETTING_COUNT;
		if (pressed & PAD_DOWN)
			synth_settings.selected =
				(synth_settings.selected + 1) % SETTING_COUNT;
		int adjustment = read_adjustment(buttons);
		if (adjustment != 0 && adjust_setting(adjustment)) rebuild_envelope();
		if (pressed & PAD_CROSS) trigger_requested = 1;
		previous_buttons = buttons;

		Sequencer display_state;
		FastEnterCriticalSection();
		display_state = sequencer;
		FastExitCriticalSection();
		draw_text(&render_context, 8, 6, "WAVETABLE SYNTHESIS");
		draw_setting_text(&render_context, 20, 0, "WAVE A",
			wave_names[synth_settings.wave_a], "");
		draw_setting_text(&render_context, 31, 1, "WAVE B",
			wave_names[synth_settings.wave_b], "");
		draw_setting_number(&render_context, 42, 2, "MIDI NOTE", synth_settings.note, "");
		draw_setting_number(&render_context, 53, 3, "AMP ATTACK",
			synth_settings.amplitude_attack_ms, "ms");
		draw_setting_number(&render_context, 64, 4, "AMP RELEASE",
			synth_settings.amplitude_release_ms, "ms");
		draw_setting_number(&render_context, 75, 5, "MIX ATTACK",
			synth_settings.mix_attack_ms, "ms");
		draw_setting_number(&render_context, 86, 6, "MIX RELEASE",
			synth_settings.mix_release_ms, "ms");
		draw_setting_number(&render_context, 97, 7, "PITCH SWEEP",
			synth_settings.pitch_sweep, "st");
		draw_setting_number(&render_context, 108, 8, "PITCH CURVE",
			synth_settings.pitch_curve, "");
		draw_text(&render_context, 8, 124, "D-pad: select/adjust  L1/R1: x10");
		draw_meters(&render_context, &display_state);
		draw_waveform(&render_context, &display_state);
		draw_text(&render_context, 8, 224, "Cross: trigger");
		flip_buffers(&render_context);
	}
}
