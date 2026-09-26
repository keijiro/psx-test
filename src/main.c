#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include <psxapi.h>
#include <psxetc.h>
#include <psxgpu.h>
#include <psxpad.h>
#include <psxspu.h>

#include "synth.h"

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define OT_LENGTH 16
#define PACKET_BUFFER_LENGTH 8192
#define WAVE_DATA_ADDR 0x1010
#define WAVE_A_CHANNEL 0
#define WAVE_B_CHANNEL 1
#define VOICE_MASK ((1 << WAVE_A_CHANNEL) | (1 << WAVE_B_CHANNEL))
#define VOICE_VOLUME 0x3000
#define WAVE_SAMPLE_PEAK 14336
#define ADSR_MAX_LEVEL 0x7fff
#define ENVELOPE_TICK_RATE 1000
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

static const char *const wave_names[WAVE_COUNT] = {
	"SINE", "TRIANGLE", "SAW", "SQUARE", "NOISE"
};

static RenderContext render_context;
static volatile Sequencer sequencer = { 0, 0, 0, 0 };
static SynthSettings synth_settings;
static EnvelopeProgram envelope_programs[2];
static volatile int active_envelope_program;
static volatile int playback_active;
static int software_envelope_started;
static volatile int trigger_requested;
static uint8_t pad_buffers[2][34];
static AdjustmentRepeat adjustment_repeat;
static int16_t wave_samples[WAVE_COUNT][WAVE_SAMPLE_COUNT];
static uint32_t wave_data[(WAVE_DATA_SIZE * WAVE_COUNT) / sizeof(uint32_t)];

static void rebuild_envelope(void) {
	int next_program = active_envelope_program ^ 1;
	synth_build_program(&synth_settings, &envelope_programs[next_program]);
	FastEnterCriticalSection();
	active_envelope_program = next_program;
	FastExitCriticalSection();
}

static void set_voice_volume(int channel, int volume) {
	SPU_CH_VOL_L(channel) = volume;
	SPU_CH_VOL_R(channel) = volume;
}

static void setup_sound(void) {
	synth_build_waves(&wave_samples[0][0], (uint8_t *) wave_data);
	SpuInit();
	SpuSetTransferMode(SPU_TRANSFER_BY_DMA);
	SpuSetTransferStartAddr(WAVE_DATA_ADDR);
	SpuWrite(wave_data, sizeof(wave_data));
	SpuIsTransferCompleted(SPU_TRANSFER_WAIT);
}

static void apply_envelope_tick(int tick) {
	const EnvelopeProgram *program = &envelope_programs[active_envelope_program];
	int mix = synth_evaluate_mix(program, tick);
	uint16_t pitch = synth_evaluate_pitch(program, tick);
	set_voice_volume(WAVE_A_CHANNEL, VOICE_VOLUME * (256 - mix) / 256);
	set_voice_volume(WAVE_B_CHANNEL, VOICE_VOLUME * mix / 256);
	SPU_CH_FREQ(WAVE_A_CHANNEL) = pitch;
	SPU_CH_FREQ(WAVE_B_CHANNEL) = pitch;
	sequencer.mix = mix;
	sequencer.frequency = synth_midi_frequency_millihz(synth_settings.note) / 1000;
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

static uint16_t adjustment_buttons(uint16_t buttons) {
	uint16_t held = 0;
	if (!(buttons & PAD_LEFT)) held |= SYNTH_ADJUST_LEFT;
	if (!(buttons & PAD_RIGHT)) held |= SYNTH_ADJUST_RIGHT;
	if (!(buttons & PAD_L1)) held |= SYNTH_ADJUST_FAST_LEFT;
	if (!(buttons & PAD_R1)) held |= SYNTH_ADJUST_FAST_RIGHT;
	return held;
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
	synth_default_settings(&synth_settings);
	setup_rendering(&render_context);
	setup_sound();
	synth_build_program(&synth_settings, &envelope_programs[0]);
	InitPAD(pad_buffers[0], sizeof(pad_buffers[0]), pad_buffers[1], sizeof(pad_buffers[1]));
	StartPAD();
	uint16_t previous_buttons = 0xffff;
	setup_envelope_timer();

	for (;;) {
		PADTYPE *pad = (PADTYPE *) pad_buffers[0];
		uint16_t buttons = pad->stat == 0 ? pad->btn : 0xffff;
		uint16_t pressed = previous_buttons & ~buttons;
		if (pressed & PAD_UP)
			synth_select_setting(&synth_settings, -1);
		if (pressed & PAD_DOWN)
			synth_select_setting(&synth_settings, 1);
		int adjustment = synth_read_adjustment(&adjustment_repeat, adjustment_buttons(buttons));
		if (adjustment != 0 && synth_adjust_setting(&synth_settings, adjustment)) rebuild_envelope();
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
