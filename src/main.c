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
#define SAW_CHANNEL       1
#define VOICE_MASK        ((1 << SINE_CHANNEL) | (1 << SAW_CHANNEL))
#define VOICE_VOLUME      0x2800

#define ATTACK_FRAMES     3
#define RELEASE_FRAMES    6
#define SAW_HOLD_FRAMES   6

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
	uint16_t frequency;
	uint8_t duration;
	const char *name;
} Note;

typedef struct {
	int note_index;
	int note_frame;
	int morph;
	int amplitude;
} Sequencer;

static const int8_t sine_samples[WAVE_SAMPLE_COUNT] = {
	 0,  1,  2,  2,  3,  4,  4,  5,  5,  6,  6,  7,  7,  7,
	 7,  7,  7,  7,  6,  6,  5,  5,  4,  4,  3,  2,  2,  1,
	 0, -1, -2, -2, -3, -4, -4, -5, -5, -6, -6, -7, -7, -7,
	-7, -7, -7, -7, -6, -6, -5, -5, -4, -4, -3, -2, -2, -1
};

// The falling ramp has the same fundamental phase as the sine table. Keeping
// both tables phase-aligned prevents the crossfade from introducing a large
// volume dip that would obscure the timbre change being demonstrated.
static const int8_t saw_samples[WAVE_SAMPLE_COUNT] = {
	 7,  7,  6,  6,  6,  6,  6,  5,  5,  5,  4,  4,  4,  4,
	 4,  3,  3,  3,  2,  2,  2,  2,  2,  1,  1,  1,  0,  0,
	 0,  0,  0, -1, -1, -1, -1, -2, -2, -2, -3, -3, -3, -3,
	-4, -4, -4, -4, -4, -5, -5, -5, -6, -6, -6, -6, -6, -7
};

static const Note phrase[] = {
	{ 262, 30, "C4" },
	{ 330, 30, "E4" },
	{ 392, 30, "G4" },
	{ 494, 45, "B4" },
	{ 440, 30, "A4" },
	{ 392, 30, "G4" },
	{ 330, 30, "E4" },
	{ 294, 45, "D4" }
};

#define PHRASE_LENGTH ((int) (sizeof(phrase) / sizeof(phrase[0])))

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
	encode_wavetable(&data[WAVE_DATA_SIZE], saw_samples);

	SpuInit();
	SpuSetTransferMode(SPU_TRANSFER_BY_DMA);
	SpuSetTransferStartAddr(WAVE_DATA_ADDR);
	SpuWrite(wave_data, sizeof(wave_data));
	SpuIsTransferCompleted(SPU_TRANSFER_WAIT);

	SPU_CH_ADDR(SINE_CHANNEL) = getSPUAddr(WAVE_DATA_ADDR);
	SPU_CH_LOOP_ADDR(SINE_CHANNEL) = getSPUAddr(WAVE_DATA_ADDR);
	SPU_CH_ADDR(SAW_CHANNEL) = getSPUAddr(WAVE_DATA_ADDR + WAVE_DATA_SIZE);
	SPU_CH_LOOP_ADDR(SAW_CHANNEL) = getSPUAddr(WAVE_DATA_ADDR + WAVE_DATA_SIZE);

	for (int channel = SINE_CHANNEL; channel <= SAW_CHANNEL; channel++) {
		set_voice_volume(channel, 0);
		SPU_CH_ADSR1(channel) = 0x00ff;
		SPU_CH_ADSR2(channel) = 0x0000;
	}
}

static void start_note(int note_index) {
	const Note *note = &phrase[note_index];
	uint16_t pitch = getSPUSampleRate(note->frequency * WAVE_SAMPLE_COUNT);

	SpuSetKey(0, VOICE_MASK);
	set_voice_volume(SINE_CHANNEL, 0);
	set_voice_volume(SAW_CHANNEL, 0);
	SPU_CH_FREQ(SINE_CHANNEL) = pitch;
	SPU_CH_FREQ(SAW_CHANNEL) = pitch;

	sequencer.note_index = note_index;
	sequencer.note_frame = 0;
	sequencer.morph = 0;
	sequencer.amplitude = 0;
	SpuSetKey(1, VOICE_MASK);
}

static void restart_phrase(void) {
	start_note(0);
}

static void update_sound(void) {
	const Note *note = &phrase[sequencer.note_index];
	int frame = sequencer.note_frame;
	int remaining = note->duration - frame;
	int morph_frames = note->duration - RELEASE_FRAMES - SAW_HOLD_FRAMES;

	if (frame < ATTACK_FRAMES)
		sequencer.amplitude = frame * 256 / ATTACK_FRAMES;
	else if (remaining <= RELEASE_FRAMES)
		sequencer.amplitude = (remaining - 1) * 256 / (RELEASE_FRAMES - 1);
	else
		sequencer.amplitude = 256;

	if (frame < morph_frames)
		sequencer.morph = frame * 256 / morph_frames;
	else
		sequencer.morph = 256;

	// These complementary gain envelopes are the wavetable interpolation:
	// sine * (1 - morph) + saw * morph. Both voices share pitch and key-on, so
	// their samples remain locked while only their balance changes.
	int volume = VOICE_VOLUME * sequencer.amplitude / 256;
	int sine_volume = volume * (256 - sequencer.morph) / 256;
	int saw_volume = volume * sequencer.morph / 256;
	set_voice_volume(SINE_CHANNEL, sine_volume);
	set_voice_volume(SAW_CHANNEL, saw_volume);

	sequencer.note_frame++;
	if (sequencer.note_frame >= note->duration)
		start_note((sequencer.note_index + 1) % PHRASE_LENGTH);
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
	int sine_width = width * (256 - sequencer.morph) / 256;

	draw_tile(context, 3, x, y, width, 9, 28, 34, 48);
	draw_tile(context, 2, x, y, sine_width, 9, 60, 150, 255);
	draw_tile(context, 2, x + sine_width, y, width - sine_width, 9, 255, 116, 48);
	draw_text(context, 8, 85, "SINE");
	draw_text(context, 273, 85, "SAW");

	draw_tile(context, 3, x, 107, width, 4, 28, 34, 48);
	draw_tile(context, 2, x, 107, width * sequencer.amplitude / 256, 4, 88, 224, 128);
	draw_text(context, 8, 103, "AMP");
}

static void draw_waveform(RenderContext *context) {
	const int graph_left = 19;
	const int graph_width = 282;
	const int center_y = 166;
	int red = 60 + 195 * sequencer.morph / 256;
	int green = 150 - 34 * sequencer.morph / 256;
	int blue = 255 - 207 * sequencer.morph / 256;

	draw_tile(context, 3, graph_left, center_y, graph_width, 1, 38, 48, 66);

	for (int index = 0; index < WAVE_SAMPLE_COUNT - 1; index++) {
		int sample_a = (
			sine_samples[index] * (256 - sequencer.morph) +
			saw_samples[index] * sequencer.morph
		) / 256;
		int sample_b = (
			sine_samples[index + 1] * (256 - sequencer.morph) +
			saw_samples[index + 1] * sequencer.morph
		) / 256;
		LINE_F2 *line = (LINE_F2 *) allocate_primitive(context, 2, sizeof(LINE_F2));
		setLineF2(line);
		setXY2(
			line,
			graph_left + index * graph_width / (WAVE_SAMPLE_COUNT - 1),
			center_y - sample_a * 6,
			graph_left + (index + 1) * graph_width / (WAVE_SAMPLE_COUNT - 1),
			center_y - sample_b * 6
		);
		setRGB0(line, red, green, blue);
	}
}

int main(void) {
	setup_rendering(&render_context);
	setup_sound();
	InitPAD(pad_buffers[0], sizeof(pad_buffers[0]), pad_buffers[1], sizeof(pad_buffers[1]));
	StartPAD();
	restart_phrase();

	uint16_t previous_buttons = 0xffff;

	for (;;) {
		update_sound();

		PADTYPE *pad = (PADTYPE *) pad_buffers[0];
		uint16_t buttons = (pad->stat == 0) ? pad->btn : 0xffff;
		if ((previous_buttons & PAD_CROSS) && !(buttons & PAD_CROSS))
			restart_phrase();
		previous_buttons = buttons;

		draw_text(&render_context, 8, 10, "SPU WAVETABLE MORPH");
		draw_text(&render_context, 8, 27, "Two phase-locked voices, one note");
		draw_text(&render_context, 8, 43, "Complementary envelopes interpolate");
		draw_text(&render_context, 8, 62, "NOTE");
		draw_text(&render_context, 54, 62, phrase[sequencer.note_index].name);
		draw_balance(&render_context);
		draw_waveform(&render_context);
		draw_text(&render_context, 8, 217, "Cross: restart phrase");

		flip_buffers(&render_context);
	}
}
