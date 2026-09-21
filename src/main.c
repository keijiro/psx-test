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
#define SQUARE_SIZE   36
#define TEST_TONE_ADDR 0x1010
#define TEST_TONE_CHANNEL 0
#define TEST_TONE_BLOCK_COUNT 4

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

static RenderContext render_context;
static uint8_t pad_buffers[2][34];
static uint32_t test_tone_data[TEST_TONE_BLOCK_COUNT * 4];
static int test_tone_frames;

static void setup_sound(void) {
	uint8_t *data = (uint8_t *) test_tone_data;

	// A filter-free ADPCM block can represent a square wave using constant
	// four-bit samples. Looping four such blocks keeps the test tone independent
	// of external assets while still exercising SPU RAM transfer and playback.
	for (int block_index = 0; block_index < TEST_TONE_BLOCK_COUNT; block_index++) {
		uint8_t *block = &data[block_index * 16];
		block[0] = 0x00;
		block[1] = (block_index == 0) ? 0x04 : 0x00;

		for (int byte_index = 0; byte_index < 7; byte_index++)
			block[2 + byte_index] = 0x77;
		for (int byte_index = 7; byte_index < 14; byte_index++)
			block[2 + byte_index] = 0x88;
	}
	data[(TEST_TONE_BLOCK_COUNT - 1) * 16 + 1] = 0x03;

	SpuInit();
	SpuSetTransferMode(SPU_TRANSFER_BY_DMA);
	SpuSetTransferStartAddr(TEST_TONE_ADDR);
	SpuWrite(test_tone_data, sizeof(test_tone_data));
	SpuIsTransferCompleted(SPU_TRANSFER_WAIT);

	SPU_CH_ADDR(TEST_TONE_CHANNEL) = getSPUAddr(TEST_TONE_ADDR);
	SPU_CH_LOOP_ADDR(TEST_TONE_CHANNEL) = getSPUAddr(TEST_TONE_ADDR);
	SPU_CH_VOL_L(TEST_TONE_CHANNEL) = 0x1800;
	SPU_CH_VOL_R(TEST_TONE_CHANNEL) = 0x1800;
	SPU_CH_ADSR1(TEST_TONE_CHANNEL) = 0x00ff;
	SPU_CH_ADSR2(TEST_TONE_CHANNEL) = 0x0000;
}

static void play_test_tone(void) {
	SpuSetKey(0, 1 << TEST_TONE_CHANNEL);
	SPU_CH_FREQ(TEST_TONE_CHANNEL) = getSPUSampleRate(11025);
	SpuSetKey(1, 1 << TEST_TONE_CHANNEL);
	test_tone_frames = 15;
}

static void update_sound(void) {
	if (test_tone_frames == 0)
		return;

	test_tone_frames--;
	if (test_tone_frames == 10)
		SPU_CH_FREQ(TEST_TONE_CHANNEL) = getSPUSampleRate(13888);
	else if (test_tone_frames == 5)
		SPU_CH_FREQ(TEST_TONE_CHANNEL) = getSPUSampleRate(16537);
	else if (test_tone_frames == 0)
		SpuSetKey(0, 1 << TEST_TONE_CHANNEL);
}

static void setup_rendering(RenderContext *context) {
	ResetGraph(0);
	FntLoad(960, 0);

	SetDefDrawEnv(&context->buffers[0].draw_env, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
	SetDefDispEnv(&context->buffers[0].disp_env, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
	SetDefDrawEnv(&context->buffers[1].draw_env, 0, SCREEN_HEIGHT, SCREEN_WIDTH, SCREEN_HEIGHT);
	SetDefDispEnv(&context->buffers[1].disp_env, 0, SCREEN_HEIGHT, SCREEN_WIDTH, SCREEN_HEIGHT);

	for (int index = 0; index < 2; index++) {
		setRGB0(&context->buffers[index].draw_env, 8, 32, 56);
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

static int clamp(int value, int minimum, int maximum) {
	if (value < minimum)
		return minimum;
	if (value > maximum)
		return maximum;
	return value;
}

int main(void) {
	setup_rendering(&render_context);
	setup_sound();
	InitPAD(pad_buffers[0], sizeof(pad_buffers[0]), pad_buffers[1], sizeof(pad_buffers[1]));
	StartPAD();
	play_test_tone();

	int x = 32;
	int y = 96;
	int velocity_x = 1;
	int frame = 0;
	uint16_t previous_buttons = 0xffff;

	for (;;) {
		update_sound();

		x += velocity_x;
		if ((x <= 0) || (x >= SCREEN_WIDTH - SQUARE_SIZE)) {
			velocity_x = -velocity_x;
			x = clamp(x, 0, SCREEN_WIDTH - SQUARE_SIZE);
		}

		PADTYPE *pad = (PADTYPE *) pad_buffers[0];
		uint16_t buttons = (pad->stat == 0) ? pad->btn : 0xffff;
		if (!(buttons & PAD_LEFT))
			x -= 2;
		if (!(buttons & PAD_RIGHT))
			x += 2;
		if (!(buttons & PAD_UP))
			y -= 2;
		if (!(buttons & PAD_DOWN))
			y += 2;
		if ((previous_buttons & PAD_CROSS) && !(buttons & PAD_CROSS))
			play_test_tone();
		previous_buttons = buttons;

		x = clamp(x, 0, SCREEN_WIDTH - SQUARE_SIZE);
		y = clamp(y, 56, SCREEN_HEIGHT - SQUARE_SIZE);

		TILE *square = (TILE *) allocate_primitive(&render_context, 1, sizeof(TILE));
		setTile(square);
		setXY0(square, x, y);
		setWH(square, SQUARE_SIZE, SQUARE_SIZE);
		setRGB0(square, 255, 192 + ((frame >> 3) & 63), 48);

		draw_text(&render_context, 8, 12, "PSn00bSDK validated");
		draw_text(&render_context, 8, 26, "D-pad / arrow keys: move");
		draw_text(&render_context, 8, 40, "Cross button: play test sound");

		flip_buffers(&render_context);
		frame++;
	}
}
