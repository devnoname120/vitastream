#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "uac.h"

typedef int SceUID;
typedef int SceInt32;
typedef int16_t AudioPcmFrame[UAC_CHANNEL_COUNT];
#define KERNEL_PID 0x10005
#define SCE_UDCD_ERROR_INVALID_ARGUMENT (-2)
#define SCE_UDCD_ERROR_DRIVER_IN_PROGRESS (-3)
#define SCE_EVENT_WAITOR 1
#define SCE_EVENT_WAITCLEAR_PAT 2

static int g_audio_state_mutex;
static SceInt32 g_audio_initialized, g_audio_shutdown, g_audio_desired;
static SceInt32 g_audio_generation, g_capture_unsafe;
static SceUID g_state_event_id = 1, g_capture_event_id = 2, g_usb_event_id = 3;
static struct MockAudioMemory {AudioPcmFrame capture[2][512];} memory;
static struct MockAudioMemory *g_audio_memory = &memory;
static unsigned char firmware_data[0xBD24];
static struct {
	int mutex, stopped, validate_calls, lookup_calls, mode_calls, disable_calls;
	int wake_calls, reset_calls, read_calls, drain_calls, pushed;
	int fail_wait, fail_validate, fail_lookup, fail_mode, fail_rate, fail_channels;
	int fail_read, fail_drain, fail_disable, stop_after_read;
	int speakers_on, fail_relay_start, fail_relay_write, fail_relay_stop;
	int relay_starts, relay_writes, relay_stops, relay_pending;
	void *pending, *completed;
} mock;

static int ksceKernelAtomicGetAndAdd32(SceInt32 *value, int add) {int old = *value; *value += add; return old;}
static void ksceKernelAtomicSet32(SceInt32 *value, int set) {*value = set;}
static int ksceKernelAtomicAddAndGet32(SceInt32 *value, int add) {return *value += add;}
static void ksceKernelLockFastMutex(int *mutex) {(void)mutex; assert(!mock.mutex); mock.mutex = 1;}
static void ksceKernelUnlockFastMutex(int *mutex) {(void)mutex; assert(mock.mutex); mock.mutex = 0;}
static int ksceKernelPollEventFlag(int id, int pattern, int flags, unsigned int *bits)
{(void)id; (void)pattern; (void)flags; *bits = 0; return 0;}
static void ksceKernelSetEventFlag(int id, int pattern) {(void)id; (void)pattern; mock.wake_calls++;}
static void ksceKernelDcacheInvalidateRange(void *data, unsigned int size)
{assert((data == memory.capture[0] || data == memory.capture[1]) && size == sizeof(memory.capture[0]));}
static void diagnostic_record(const char *stage, int value) {(void)stage; (void)value;}
static void audio_request_stop_locked(void) {assert(mock.mutex); g_audio_desired = 0;}
static int audio_wait_stopped(int generation)
{assert(mock.mutex && !g_audio_desired && generation == g_audio_generation); mock.stopped++; return mock.fail_wait ? -71 : 0;}
static int firmware_audio_data(uintptr_t *address)
{assert(mock.stopped && mock.mutex); mock.validate_calls++; *address = (uintptr_t)firmware_data; return mock.fail_validate ? -72 : 0;}
static void audio_discard_stale_worker_events(void) {assert(mock.stopped && mock.mutex);}
static void audio_ring_reset(void) {assert(mock.stopped && mock.mutex); mock.reset_calls++;}
static void audio_signal_failure(void) {g_audio_desired = 0; g_audio_generation++;}
static void audio_ring_push(const AudioPcmFrame *frames, unsigned int count)
{
	assert((const void *)frames == mock.completed && (const void *)frames != mock.pending);
	assert(count == 512 && frames[0][0] == 123 && frames[0][1] == -123);
	mock.pushed++;
}
static int vitastream_settings_speakers_on(void) {return mock.speakers_on;}
static int audio_relay_start(void)
{
	assert(!mock.mutex);
	mock.relay_starts++; return mock.fail_relay_start ? -81 : 0;
}
static int audio_relay_write(const void *pcm, size_t bytes)
{
	assert(pcm == mock.completed && bytes == sizeof(memory.capture[0]) && mock.pushed);
	mock.relay_writes++; mock.relay_pending = mock.fail_relay_write;
	return mock.fail_relay_write ? -82 : 0;
}
static int audio_relay_stop(void)
{
	mock.relay_stops++;
	if (mock.fail_relay_stop) return -83;
	mock.relay_pending = 0; return 0;
}
static int mock_set_mode(int path, int mode);
static int mock_set_rate(int rate);
static int mock_set_channels(int channels);
static int mock_monitor_read(int path, void *pcm, int frames);
int module_get_export_func(SceUID pid, const char *module, uint32_t library,
	uint32_t function, uintptr_t *address);

#include "audio-backend.inc"

static int mock_set_mode(int path, int mode)
{
	assert(path == AUDIO_MONITOR_PATH);
	if (mode == AUDIO_MONITOR_MODE_DISABLED) {
		assert(!mock.relay_pending);
		mock.disable_calls++;
		return mock.fail_disable ? -80 : 0;
	}
	assert(mode == AUDIO_MONITOR_MODE_READBACK && mock.stopped && mock.lookup_calls == 4);
	mock.mode_calls++;
	return mock.fail_mode ? -74 : 0;
}
static int mock_set_rate(int rate) {assert(rate == 48000 && mock.mode_calls); return mock.fail_rate ? -75 : 0;}
static int mock_set_channels(int channels) {assert(channels == 2 && mock.mode_calls); return mock.fail_channels ? -76 : 0;}
static int mock_monitor_read(int path, void *pcm, int frames)
{
	assert(path == 0 && frames == 512);
	if (!pcm) {
		assert(mock.pending);
		mock.drain_calls++;
		if (mock.fail_drain) return -78;
		mock.completed = mock.pending; mock.pending = NULL;
		return 0;
	}
	assert(pcm == memory.capture[0] || pcm == memory.capture[1]);
	assert(pcm != mock.pending);
	mock.read_calls++;
	if (mock.read_calls == mock.fail_read) return -77;
	/* A successful next read fences the previous destination before queueing. */
	mock.completed = mock.pending;
	if (mock.completed) {
		AudioPcmFrame *samples = mock.completed;
		samples[0][0] = 123; samples[0][1] = -123;
	}
	mock.pending = pcm;
	if (mock.read_calls == mock.stop_after_read) g_audio_desired = 0;
	return 0;
}
int module_get_export_func(SceUID pid, const char *module, uint32_t library,
	uint32_t function, uintptr_t *address)
{
	const uint32_t nids[] = {SCE_AUDIO_MONITOR_SET_PATH_MODE_NID, SCE_AUDIO_MONITOR_SET_RATE_NID,
		SCE_AUDIO_MONITOR_SET_CHANNEL_MODE_NID, SCE_AUDIO_MONITOR_READ_NID};
	const uintptr_t exports[] = {(uintptr_t)mock_set_mode, (uintptr_t)mock_set_rate,
		(uintptr_t)mock_set_channels, (uintptr_t)mock_monitor_read};
	assert(pid == KERNEL_PID && !strcmp(module, "SceAudio") && library == SCE_AUDIO_FOR_DRIVER_NID);
	assert(mock.lookup_calls < 4 && function == nids[mock.lookup_calls]);
	mock.lookup_calls++;
	if (mock.lookup_calls == mock.fail_lookup) return -73;
	*address = exports[mock.lookup_calls - 1]; return 0;
}
static void reset(void)
{
	memset(&mock, 0, sizeof(mock)); memset(firmware_data, 0, sizeof(firmware_data));
	memset(&memory, 0, sizeof(memory)); firmware_data[AUDIO_SRCMIX0_MODE_OFFSET] = 0xFE;
	g_audio_initialized = 1; g_audio_shutdown = g_audio_desired = g_capture_unsafe = 0;
	g_audio_generation = 7;
	g_audio_monitor_set_path_mode = NULL; g_audio_monitor_set_sample_rate = NULL;
	g_audio_monitor_set_channel_mode = NULL; g_audio_monitor_read = NULL;
	g_audio_relay_enabled = 0;
}
int main(void)
{
	reset(); g_audio_initialized = 0;
	assert(uac_audio_start() == -2 && !mock.stopped && !mock.mutex);
	reset(); g_audio_shutdown = 1;
	assert(uac_audio_start() == -3 && !mock.stopped && !mock.mutex);
	reset(); g_audio_desired = 1;
	assert(uac_audio_start() == 0 && !mock.stopped && !mock.mode_calls && !mock.mutex);
	reset(); mock.fail_wait = 1;
	assert(uac_audio_start() == -71 && !mock.validate_calls && !mock.mode_calls && !mock.mutex);
	reset(); mock.fail_validate = 1;
	assert(uac_audio_start() == -72 && !mock.lookup_calls && !mock.mutex);
	for (int owner = 0; owner < 4; owner++) {
		reset();
		firmware_data[owner < 2 ? AUDIO_SRCMIX0_MODE_OFFSET : AUDIO_SRCMIX1_MODE_OFFSET] = owner % 2 + 1;
		assert(uac_audio_start() == -3 && !mock.lookup_calls && !mock.mode_calls && !mock.mutex);
	}
	for (int export = 1; export <= 4; export++) {
		reset(); mock.fail_lookup = export;
		assert(uac_audio_start() == -73 && !mock.mode_calls && !mock.mutex);
		assert(!g_audio_monitor_set_path_mode && !g_audio_monitor_set_sample_rate);
		assert(!g_audio_monitor_set_channel_mode && !g_audio_monitor_read);
	}
	reset(); mock.fail_mode = 1;
	assert(uac_audio_start() == -74 && !g_audio_desired && !mock.reset_calls && !mock.mutex);
	reset(); mock.fail_rate = 1;
	assert(uac_audio_start() == -75 && !g_audio_desired && mock.disable_calls == 1 && !mock.mutex);
	reset(); mock.fail_channels = 1;
	assert(uac_audio_start() == -76 && !g_audio_desired && mock.disable_calls == 1 && !mock.mutex);
	reset(); assert(uac_audio_start() == 0);
	assert(g_audio_desired && g_audio_generation == 8 && mock.reset_calls == 1 && mock.wake_calls == 2 && !mock.mutex);
	assert(uac_audio_start() == 0 && mock.mode_calls == 1 && mock.lookup_calls == 4);
	reset(); mock.speakers_on = 1; mock.fail_relay_start = 1;
	assert(uac_audio_start() == 0 && !mock.relay_starts && mock.mode_calls == 1);
	assert(audio_capture_run(8) == 0 && mock.relay_starts == 1 && !mock.read_calls && mock.disable_calls == 1 && !g_audio_desired);
	reset(); mock.speakers_on = 1; mock.fail_rate = 1;
	assert(uac_audio_start() == -75 && !mock.relay_starts && !g_audio_relay_enabled);
	reset(); mock.speakers_on = 1;
	assert(uac_audio_start() == 0 && !mock.relay_starts && g_audio_relay_enabled);

	for (int failure = 0; failure < 5; failure++) {
		reset(); g_audio_desired = 1; mock.stop_after_read = 3;
		g_audio_monitor_set_path_mode = mock_set_mode; g_audio_monitor_read = mock_monitor_read;
		mock.fail_read = failure == 1 ? 1 : failure == 2 ? 2 : 0;
		mock.fail_drain = failure == 3; mock.fail_disable = failure == 4;
		int result = audio_capture_run(7);
		assert(result == (failure == 3 ? -78 : failure == 4 ? -80 : 0));
		assert(!g_audio_desired && mock.disable_calls == 1);
		assert(mock.drain_calls == (failure != 1));
		assert(mock.pushed == (failure == 0 || failure >= 3));
		assert(g_capture_unsafe == (failure == 3));
		assert(!mock.pending || failure == 3);
	}
	reset(); g_audio_desired = g_audio_relay_enabled = 1; mock.stop_after_read = 3;
	g_audio_monitor_set_path_mode = mock_set_mode; g_audio_monitor_read = mock_monitor_read;
	assert(audio_capture_run(7) == 0 && mock.relay_writes == 1 && mock.relay_stops == 1 && !g_audio_relay_enabled);
	reset(); g_audio_desired = g_audio_relay_enabled = 1; mock.fail_relay_write = 1;
	g_audio_monitor_set_path_mode = mock_set_mode; g_audio_monitor_read = mock_monitor_read;
	assert(audio_capture_run(7) == 0 && mock.relay_writes == 1 && !mock.relay_pending && mock.disable_calls == 1);
	reset(); g_audio_desired = g_audio_relay_enabled = 1; mock.fail_relay_write = mock.fail_relay_stop = 1;
	g_audio_monitor_set_path_mode = mock_set_mode; g_audio_monitor_read = mock_monitor_read;
	assert(audio_capture_run(7) == -83 && g_capture_unsafe && mock.relay_pending);
	assert(!mock.disable_calls && !mock.drain_calls); /* never race the normal I2S writer */
	puts("audio: full-mix export resolution, startup ordering/ownership, stop fences and failure cleanup passed");
	return 0;
}
