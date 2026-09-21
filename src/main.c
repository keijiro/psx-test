#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include <psxapi.h>
#include <psxgpu.h>
#include <psxpad.h>
#include <psxspu.h>

#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 240
#define OT_LENGTH     16
#define PACKET_BUFFER_LENGTH 8192

#define WAVE_SAMPLE_COUNT 56
#define WAVE_BLOCK_COUNT  2
#define WAVE_DATA_SIZE    (WAVE_BLOCK_COUNT * 16)
#define WAVE_DATA_ADDR    0x1010
#define SINE_CHANNEL      0
#define NOISE_CHANNEL     1
#define VOICE_MASK        ((1 << SINE_CHANNEL) | (1 << NOISE_CHANNEL))
#define VOICE_VOLUME      0x3000

#define KICK_INTERVAL          30
#define AMPLITUDE_SHAPE_FRAMES 24
#define PITCH_SHAPE_FRAMES     12
#define NOISE_SHAPE_FRAMES     4
#define SETTING_COUNT          5

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
	int kick_frame;
	int noise_mix;
	int amplitude;
	int frequency;
} Sequencer;

typedef struct {
	int pitch_start;
	int pitch_end;
	int pitch_frames;
	int amplitude_frames;
	int noise_frames;
	int selected;
} EnvelopeSettings;

static const int8_t sine_samples[WAVE_SAMPLE_COUNT] = {
	 0,  1,  2,  2,  3,  4,  4,  5,  5,  6,  6,  7,  7,  7,
	 7,  7,  7,  7,  6,  6,  5,  5,  4,  4,  3,  2,  2,  1,
	 0, -1, -2, -2, -3, -4, -4, -5, -5, -6, -6, -7, -7, -7,
	-7, -7, -7, -7, -6, -6, -5, -5, -4, -4, -3, -2, -2, -1
};

// This fixed, zero-centered noise cycle makes the experiment deterministic.
// It repeats like any other wavetable, but it disappears before the period is
// perceived as a pitched tone.
static const int8_t noise_samples[WAVE_SAMPLE_COUNT] = {
	 6, -4,  2,  7, -3, -6,  5, -1, -5,  3,  6, -4,  1, -7,
	 7, -2, -5,  4, -8,  6,  0, -3,  5, -6,  2,  7, -4, -1,
	-7,  4,  1, -5,  7, -3, -5,  5, -2,  6, -6,  3,  0, -7,
	 5, -4,  7, -1, -5,  2,  6, -8,  4, -2, -6,  7,  1,  1
};

// At 60 Hz these first four steps make the transient change from noise to
// sine in about 50 ms. The pitch drops an octave and a half in about 100 ms,
// while the longer nonlinear level decay leaves the sine body audible.
static const uint16_t pitch_shape[PITCH_SHAPE_FRAMES] = {
	180, 145, 116, 94, 78, 66, 58, 53, 50, 48, 47, 46
};

static const uint16_t amplitude_shape[AMPLITUDE_SHAPE_FRAMES] = {
	256, 250, 239, 225, 208, 190, 172, 154,
	137, 121, 106,  92,  79,  67,  56,  46,
	 37,  29,  22,  16,  11,   7,   3,   0
};

static const uint16_t noise_shape[NOISE_SHAPE_FRAMES] = {
	256, 112, 32, 0
};

static RenderContext render_context;
static Sequencer sequencer;
static EnvelopeSettings envelope_settings = { 180, 46, 12, 24, 4, 0 };
static uint8_t pad_buffers[2][34];
static uint32_t wave_data[(WAVE_DATA_SIZE * 2) / sizeof(uint32_t)];

static int clamp(int value, int minimum, int maximum) {
	if (value < minimum)
		return minimum;
	if (value > maximum)
		return maximum;
	return value;
}

static int sample_shape(
	const uint16_t *shape, int shape_frames, int output_frames, int frame
) {
	if (frame >= output_frames)
		return shape[shape_frames - 1];
	if (output_frames == 1)
		return shape[0];

	// Linear interpolation lets timing change without replacing the measured
	// curves, and gives the original table back exactly at its default length.
	int position = frame * (shape_frames - 1) * 256 / (output_frames - 1);
	int index = position / 256;
	int fraction = position & 255;
	if (index >= shape_frames - 1)
		return shape[shape_frames - 1];

	return (shape[index] * (256 - fraction) + shape[index + 1] * fraction) / 256;
}

static int evaluate_pitch(int frame) {
	int shape = sample_shape(
		pitch_shape, PITCH_SHAPE_FRAMES, envelope_settings.pitch_frames, frame
	);
	return envelope_settings.pitch_end +
		(shape - pitch_shape[PITCH_SHAPE_FRAMES - 1]) *
		(envelope_settings.pitch_start - envelope_settings.pitch_end) /
		(pitch_shape[0] - pitch_shape[PITCH_SHAPE_FRAMES - 1]);
}

static void adjust_envelope_setting(int direction) {
	switch (envelope_settings.selected) {
		case 0:
			envelope_settings.pitch_start = clamp(
				envelope_settings.pitch_start + direction * 10,
				envelope_settings.pitch_end + 10, 300
			);
			break;
		case 1:
			envelope_settings.pitch_end = clamp(
				envelope_settings.pitch_end + direction * 2, 30,
				envelope_settings.pitch_start - 10
			);
			break;
		case 2:
			envelope_settings.pitch_frames = clamp(
				envelope_settings.pitch_frames + direction, 4, KICK_INTERVAL
			);
			break;
		case 3:
			envelope_settings.amplitude_frames = clamp(
				envelope_settings.amplitude_frames + direction, 4, KICK_INTERVAL
			);
			break;
		case 4:
			envelope_settings.noise_frames = clamp(
				envelope_settings.noise_frames + direction, 1, 12
			);
			break;
	}
}

static void encode_wavetable(uint8_t *destination, const int8_t *samples) {
	// Two 28-sample, filter-free ADPCM blocks make one complete cycle. The
	// small table deliberately uses native four-bit levels so runtime encoding
	// is exact and the demo has no external asset or host-side build step.
	for (int block_index = 0; block_index < WAVE_BLOCK_COUNT; block_index++) {
		uint8_t *block = &destination[block_index * 16];
		block[0] = 0x01;
		block[1] = (block_index == 0) ? 0x04 : 0x03;

		for (int byte_index = 0; byte_index < 14; byte_index++) {
			int sample_index = block_index * 28 + byte_index * 2;
			uint8_t low = (uint8_t) samples[sample_index] & 0x0f;
			uint8_t high = (uint8_t) samples[sample_index + 1] & 0x0f;
			block[byte_index + 2] = low | (high << 4);
		}
	}
}

static void set_voice_volume(int channel, int volume) {
	SPU_CH_VOL_L(channel) = volume;
	SPU_CH_VOL_R(channel) = volume;
}

static void setup_sound(void) {
	uint8_t *data = (uint8_t *) wave_data;
	encode_wavetable(data, sine_samples);
	encode_wavetable(&data[WAVE_DATA_SIZE], noise_samples);

	SpuInit();
	SpuSetTransferMode(SPU_TRANSFER_BY_DMA);
	SpuSetTransferStartAddr(WAVE_DATA_ADDR);
	SpuWrite(wave_data, sizeof(wave_data));
	SpuIsTransferCompleted(SPU_TRANSFER_WAIT);

	SPU_CH_ADDR(SINE_CHANNEL) = getSPUAddr(WAVE_DATA_ADDR);
	SPU_CH_LOOP_ADDR(SINE_CHANNEL) = getSPUAddr(WAVE_DATA_ADDR);
	SPU_CH_ADDR(NOISE_CHANNEL) = getSPUAddr(WAVE_DATA_ADDR + WAVE_DATA_SIZE);
	SPU_CH_LOOP_ADDR(NOISE_CHANNEL) = getSPUAddr(WAVE_DATA_ADDR + WAVE_DATA_SIZE);

	for (int channel = SINE_CHANNEL; channel <= NOISE_CHANNEL; channel++) {
		set_voice_volume(channel, 0);
		SPU_CH_ADSR1(channel) = 0x00ff;
		SPU_CH_ADSR2(channel) = 0x0000;
	}
}

static void start_kick(void) {
	uint16_t pitch = getSPUSampleRate(envelope_settings.pitch_start * WAVE_SAMPLE_COUNT);

	SpuSetKey(0, VOICE_MASK);
	set_voice_volume(SINE_CHANNEL, 0);
	set_voice_volume(NOISE_CHANNEL, 0);
	SPU_CH_FREQ(SINE_CHANNEL) = pitch;
	SPU_CH_FREQ(NOISE_CHANNEL) = pitch;

	sequencer.kick_frame = 0;
	sequencer.noise_mix = noise_shape[0];
	sequencer.amplitude = amplitude_shape[0];
	sequencer.frequency = envelope_settings.pitch_start;
	SpuSetKey(1, VOICE_MASK);
}

static void update_sound(void) {
	int frame = sequencer.kick_frame;

	sequencer.noise_mix = sample_shape(
		noise_shape, NOISE_SHAPE_FRAMES, envelope_settings.noise_frames, frame
	);
	sequencer.amplitude = sample_shape(
		amplitude_shape, AMPLITUDE_SHAPE_FRAMES,
		envelope_settings.amplitude_frames, frame
	);
	sequencer.frequency = evaluate_pitch(frame);

	uint16_t pitch = getSPUSampleRate(sequencer.frequency * WAVE_SAMPLE_COUNT);
	SPU_CH_FREQ(SINE_CHANNEL) = pitch;
	SPU_CH_FREQ(NOISE_CHANNEL) = pitch;

	// Complementary voice gains interpolate noise into sine. Both voices use
	// the same pitch sweep, so the transient and body remain one sound.
	int volume = VOICE_VOLUME * sequencer.amplitude / 256;
	int sine_volume = volume * (256 - sequencer.noise_mix) / 256;
	int noise_volume = volume * sequencer.noise_mix / 256;
	set_voice_volume(SINE_CHANNEL, sine_volume);
	set_voice_volume(NOISE_CHANNEL, noise_volume);

	sequencer.kick_frame++;
	if (sequencer.kick_frame >= KICK_INTERVAL)
		start_kick();
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
	while (*source)
		*destination++ = *source++;
	return destination;
}

static char *append_number(char *destination, int value) {
	int divisor = 100;
	while (divisor > 1 && value < divisor)
		divisor /= 10;
	while (divisor > 0) {
		*destination++ = '0' + value / divisor;
		value %= divisor;
		divisor /= 10;
	}
	return destination;
}

static void draw_setting(
	RenderContext *context, int y, int index, const char *label, int value,
	const char *unit
) {
	char text[32];
	char *next = text;
	*next++ = (envelope_settings.selected == index) ? '>' : ' ';
	*next++ = ' ';
	next = append_text(next, label);
	*next++ = ':';
	*next++ = ' ';
	next = append_number(next, value);
	*next++ = ' ';
	next = append_text(next, unit);
	*next = '\0';
	draw_text(context, 8, y, text);
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

static void draw_balance(RenderContext *context) {
	const int x = 54;
	const int y = 108;
	const int width = 212;
	int noise_width = width * sequencer.noise_mix / 256;

	draw_tile(context, 3, x, y, width, 9, 28, 34, 48);
	draw_tile(context, 2, x, y, noise_width, 9, 255, 116, 48);
	draw_tile(context, 2, x + noise_width, y, width - noise_width, 9, 60, 150, 255);
	draw_text(context, 8, 105, "NOISE");
	draw_text(context, 273, 105, "SINE");

	draw_tile(context, 3, x, 127, width, 4, 28, 34, 48);
	draw_tile(context, 2, x, 127, width * sequencer.amplitude / 256, 4, 88, 224, 128);
	draw_text(context, 8, 123, "AMP");

	draw_tile(context, 3, x, 142, width, 4, 28, 34, 48);
	draw_tile(
		context, 2, x, 142,
		width * clamp(sequencer.frequency, 0, envelope_settings.pitch_start) /
			envelope_settings.pitch_start,
		4, 232, 204, 72
	);
	draw_text(context, 8, 138, "PITCH");
}

static void draw_waveform(RenderContext *context) {
	const int graph_left = 19;
	const int graph_width = 282;
	const int center_y = 181;
	int red = 60 + 195 * sequencer.noise_mix / 256;
	int green = 150 - 34 * sequencer.noise_mix / 256;
	int blue = 255 - 207 * sequencer.noise_mix / 256;

	draw_tile(context, 3, graph_left, center_y, graph_width, 1, 38, 48, 66);

	for (int index = 0; index < WAVE_SAMPLE_COUNT - 1; index++) {
		int sample_a = (
			sine_samples[index] * (256 - sequencer.noise_mix) +
			noise_samples[index] * sequencer.noise_mix
		) / 256;
		int sample_b = (
			sine_samples[index + 1] * (256 - sequencer.noise_mix) +
			noise_samples[index + 1] * sequencer.noise_mix
		) / 256;
		LINE_F2 *line = (LINE_F2 *) allocate_primitive(context, 2, sizeof(LINE_F2));
		setLineF2(line);
		setXY2(
			line,
			graph_left + index * graph_width / (WAVE_SAMPLE_COUNT - 1),
			center_y - sample_a * 4,
			graph_left + (index + 1) * graph_width / (WAVE_SAMPLE_COUNT - 1),
			center_y - sample_b * 4
		);
		setRGB0(line, red, green, blue);
	}
}

int main(void) {
	setup_rendering(&render_context);
	setup_sound();
	InitPAD(pad_buffers[0], sizeof(pad_buffers[0]), pad_buffers[1], sizeof(pad_buffers[1]));
	StartPAD();
	start_kick();

	uint16_t previous_buttons = 0xffff;

	for (;;) {
		update_sound();

		PADTYPE *pad = (PADTYPE *) pad_buffers[0];
		uint16_t buttons = (pad->stat == 0) ? pad->btn : 0xffff;
		uint16_t pressed = previous_buttons & ~buttons;
		if (pressed & PAD_UP)
			envelope_settings.selected =
				(envelope_settings.selected + SETTING_COUNT - 1) % SETTING_COUNT;
		if (pressed & PAD_DOWN)
			envelope_settings.selected =
				(envelope_settings.selected + 1) % SETTING_COUNT;
		if (pressed & PAD_LEFT)
			adjust_envelope_setting(-1);
		if (pressed & PAD_RIGHT)
			adjust_envelope_setting(1);
		if ((previous_buttons & PAD_CROSS) && !(buttons & PAD_CROSS))
			start_kick();
		previous_buttons = buttons;

		draw_text(&render_context, 8, 10, "WAVETABLE KICK SYNTHESIS");
		draw_setting(
			&render_context, 27, 0, "PITCH START", envelope_settings.pitch_start, "Hz"
		);
		draw_setting(
			&render_context, 39, 1, "PITCH END", envelope_settings.pitch_end, "Hz"
		);
		draw_setting(
			&render_context, 51, 2, "PITCH SWEEP", envelope_settings.pitch_frames, "fr"
		);
		draw_setting(
			&render_context, 63, 3, "AMP DECAY", envelope_settings.amplitude_frames, "fr"
		);
		draw_setting(
			&render_context, 75, 4, "NOISE DECAY", envelope_settings.noise_frames, "fr"
		);
		draw_text(&render_context, 8, 91, "D-pad: select / adjust");
		draw_balance(&render_context);
		draw_waveform(&render_context);
		draw_text(&render_context, 8, 217, "Cross: trigger kick");

		flip_buffers(&render_context);
	}
}
