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

#define KICK_INTERVAL     30
#define ENVELOPE_FRAMES   24

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
static const uint16_t pitch_envelope[ENVELOPE_FRAMES] = {
	180, 145, 116,  94,  78,  66,  58,  53,
	 50,  48,  47,  46,  46,  46,  46,  46,
	 46,  46,  46,  46,  46,  46,  46,  46
};

static const uint16_t amplitude_envelope[ENVELOPE_FRAMES] = {
	256, 250, 239, 225, 208, 190, 172, 154,
	137, 121, 106,  92,  79,  67,  56,  46,
	 37,  29,  22,  16,  11,   7,   3,   0
};

static const uint16_t noise_envelope[ENVELOPE_FRAMES] = {
	256, 112,  32,   0,   0,   0,   0,   0,
	  0,   0,   0,   0,   0,   0,   0,   0,
	  0,   0,   0,   0,   0,   0,   0,   0
};

static RenderContext render_context;
static Sequencer sequencer;
static uint8_t pad_buffers[2][34];
static uint32_t wave_data[(WAVE_DATA_SIZE * 2) / sizeof(uint32_t)];

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
	uint16_t pitch = getSPUSampleRate(pitch_envelope[0] * WAVE_SAMPLE_COUNT);

	SpuSetKey(0, VOICE_MASK);
	set_voice_volume(SINE_CHANNEL, 0);
	set_voice_volume(NOISE_CHANNEL, 0);
	SPU_CH_FREQ(SINE_CHANNEL) = pitch;
	SPU_CH_FREQ(NOISE_CHANNEL) = pitch;

	sequencer.kick_frame = 0;
	sequencer.noise_mix = noise_envelope[0];
	sequencer.amplitude = amplitude_envelope[0];
	sequencer.frequency = pitch_envelope[0];
	SpuSetKey(1, VOICE_MASK);
}

static void update_sound(void) {
	int frame = sequencer.kick_frame;

	if (frame < ENVELOPE_FRAMES) {
		sequencer.noise_mix = noise_envelope[frame];
		sequencer.amplitude = amplitude_envelope[frame];
		sequencer.frequency = pitch_envelope[frame];
	} else {
		sequencer.noise_mix = 0;
		sequencer.amplitude = 0;
		sequencer.frequency = pitch_envelope[ENVELOPE_FRAMES - 1];
	}

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
	const int y = 88;
	const int width = 212;
	int noise_width = width * sequencer.noise_mix / 256;

	draw_tile(context, 3, x, y, width, 9, 28, 34, 48);
	draw_tile(context, 2, x, y, noise_width, 9, 255, 116, 48);
	draw_tile(context, 2, x + noise_width, y, width - noise_width, 9, 60, 150, 255);
	draw_text(context, 8, 85, "NOISE");
	draw_text(context, 273, 85, "SINE");

	draw_tile(context, 3, x, 107, width, 4, 28, 34, 48);
	draw_tile(context, 2, x, 107, width * sequencer.amplitude / 256, 4, 88, 224, 128);
	draw_text(context, 8, 103, "AMP");

	draw_tile(context, 3, x, 122, width, 4, 28, 34, 48);
	draw_tile(context, 2, x, 122, width * sequencer.frequency / 180, 4, 232, 204, 72);
	draw_text(context, 8, 118, "PITCH");
}

static void draw_waveform(RenderContext *context) {
	const int graph_left = 19;
	const int graph_width = 282;
	const int center_y = 170;
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
			center_y - sample_a * 5,
			graph_left + (index + 1) * graph_width / (WAVE_SAMPLE_COUNT - 1),
			center_y - sample_b * 5
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
		if ((previous_buttons & PAD_CROSS) && !(buttons & PAD_CROSS))
			start_kick();
		previous_buttons = buttons;

		draw_text(&render_context, 8, 10, "WAVETABLE KICK SYNTHESIS");
		draw_text(&render_context, 8, 27, "Noise transient -> sine body");
		draw_text(&render_context, 8, 43, "Fast high -> low pitch sweep");
		draw_text(&render_context, 8, 62, "RETRIGGER: 120 BPM");
		draw_balance(&render_context);
		draw_waveform(&render_context);
		draw_text(&render_context, 8, 217, "Cross: trigger kick");

		flip_buffers(&render_context);
	}
}
