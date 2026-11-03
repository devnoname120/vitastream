#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "settings.h"
#include <psp2kern/io/fcntl.h>
#include <taihen.h>

static struct {
	const char *text;
	int oled, lcd, fail_lookup, fail_read, fail_on, fail_off, fail_brightness;
	int opens, reads, closes, on, off, get_brightness, set_brightness;
	int brightness_busy, delays;
} mock;

SceUID ksceIoOpen(const char *path, int flags, int mode)
{
	assert(!strcmp(path, VITASTREAM_CONFIG_PATH) && flags == SCE_O_RDONLY && mode == 0);
	mock.opens++;
	return mock.text ? 7 : -2;
}
int ksceIoRead(SceUID fd, void *data, SceSize size)
{
	assert(fd == 7 && size == VITASTREAM_CONFIG_MAX_SIZE + 1);
	mock.reads++;
	if (mock.fail_read) return -77;
	size_t length = strlen(mock.text);
	if (length > size) length = size;
	memcpy(data, mock.text, length);
	return (int)length;
}
int ksceIoClose(SceUID fd) {assert(fd == 7); mock.closes++; return 0;}
static int panel_on(void) {mock.on++; return mock.fail_on ? -71 : 0;}
static int panel_off(void) {mock.off++; return mock.fail_off ? -72 : 0;}
static int panel_brightness(void) {mock.get_brightness++; return mock.fail_brightness ? -73 : 812;}
static int panel_set_brightness(int value)
{
	assert(value == 812); mock.set_brightness++;
	if (mock.brightness_busy) {mock.brightness_busy--; return (int)0x803F0A05;}
	return 0;
}
int ksceKernelDelayThread(SceUInt32 microseconds)
{
	assert(microseconds == 10000); mock.delays++; return 0;
}
int module_get_export_func(SceUID pid, const char *module, uint32_t library,
	uint32_t nid, uintptr_t *function)
{
	assert(pid == KERNEL_PID);
	const uint32_t oled[] = {0x4C7836C7, 0xBC84602E, 0x43EF811A, 0xF9624C47};
	const uint32_t lcd[] = {0x5F4124AB, 0x1A0A7519, 0x3A6D6AC3, 0x581D3A87};
	const uint32_t *nids;
	if (!strcmp(module, "SceOled")) {
		assert(library == 0x60C7478A); if (!mock.oled) return -1; nids = oled;
	} else {
		assert(!strcmp(module, "SceLcd") && library == 0xFA916D71);
		if (!mock.lcd) return -1; nids = lcd;
	}
	if (mock.fail_lookup) return -74;
	const uintptr_t functions[] = {(uintptr_t)panel_on, (uintptr_t)panel_off,
		(uintptr_t)panel_brightness, (uintptr_t)panel_set_brightness};
	for (unsigned int i = 0; i < 4; i++) {
		if (nid == nids[i]) {*function = functions[i]; return 0;}
	}
	assert(0); return -1;
}

int main(void)
{
	vitastream_settings_load();
	assert(!vitastream_settings_speakers_on() && !mock.reads && !mock.closes);
	assert(vitastream_screen_update(1) == 0 && !mock.off);
	mock.text = "screen=off\nspeakers=on";
	vitastream_settings_load();
	assert(vitastream_settings_speakers_on() && mock.reads == 1 && mock.closes == 1);
	assert(vitastream_screen_update(0) == 0 && !mock.off);
	assert(vitastream_screen_update(1) < 0 && !mock.off); /* no panel/TV */
	mock.oled = mock.lcd = 1;
	assert(vitastream_screen_update(1) < 0 && !mock.off); /* ambiguous, never guess */
#ifdef TEST_LCD
	mock.oled = 0;
#else
	mock.lcd = 0;
#endif
	mock.fail_lookup = 1;
	assert(vitastream_screen_update(1) == -1 && !mock.off);
	mock.fail_lookup = 0; mock.fail_brightness = 1;
	assert(vitastream_screen_update(1) == -73 && !mock.off);
	mock.fail_brightness = 0; mock.fail_off = 1;
	assert(vitastream_screen_update(1) == -72 && mock.off == 1 && !mock.on);
	mock.fail_off = 0;
	assert(vitastream_screen_update(1) == 0 && mock.off == 2);
	assert(vitastream_screen_update(1) == 0 && mock.off == 2);
	mock.fail_on = 1;
#ifdef TEST_LCD
	assert(vitastream_screen_restore() == -71 && !mock.set_brightness);
	mock.fail_on = 0;
	assert(vitastream_screen_update(0) == 0 && mock.set_brightness == 1);
	assert(vitastream_screen_update(0) == 0 && mock.set_brightness == 1);
#else
	mock.brightness_busy = 2;
	assert(vitastream_screen_restore() == 0 && !mock.on && mock.set_brightness == 3 && mock.delays == 2);
	assert(vitastream_screen_update(0) == 0 && mock.set_brightness == 3);
#endif
	mock.text = "screen=off\nspeakers=broken";
	vitastream_settings_load();
	assert(!vitastream_settings_speakers_on());
	assert(vitastream_screen_update(1) == 0 && mock.off == 2);
	mock.fail_read = 1;
	vitastream_settings_load();
	assert(!vitastream_settings_speakers_on() && mock.reads == mock.closes);
	puts("settings: file/default failures, panel selection, streaming-only blanking and restoration retries passed");
	return 0;
}
