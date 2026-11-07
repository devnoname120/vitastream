#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "settings.h"
#include <psp2kern/io/fcntl.h>
#include <psp2kern/kernel/suspend.h>
#include <taihen.h>

static struct {
	const char *text;
	int oled, lcd, fail_lookup, fail_read, fail_on, fail_off, fail_brightness;
	int opens, reads, closes, on, off, get_brightness, set_brightness;
	int brightness_busy, delays;
	int fail_open, fail_create, fail_write, zero_write, write_limit;
	int creates, writes, create_closes;
	const char *race_text;
	char created[128];
	size_t written;
	uint64_t time;
	int power_ticks, power_result;
} mock;

uint64_t ksceKernelGetSystemTimeWide(void) { return mock.time; }
int ksceKernelPowerTick(SceKernelPowerTickType type)
{
	assert(type == SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
	mock.power_ticks++;
	return mock.power_result;
}

SceUID ksceIoOpen(const char *path, int flags, int mode)
{
	assert(!strcmp(path, VITASTREAM_CONFIG_PATH));
	mock.opens++;
	if (flags == SCE_O_RDONLY) {
		assert(mode == 0);
		return mock.fail_open ? -78 : mock.text ? 7 : -2;
	}
	assert(flags == (SCE_O_WRONLY | SCE_O_CREAT | SCE_O_EXCL) && mode == 0666);
	mock.creates++;
	if (mock.race_text)
		mock.text = mock.race_text;
	if (mock.text)
		return -17;
	if (mock.fail_create)
		return -76;
	mock.text = mock.created;
	return 8;
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
int ksceIoWrite(SceUID fd, const void *data, SceSize size)
{
	assert(fd == 8 && mock.text == mock.created);
	mock.writes++;
	if (mock.fail_write) return -79;
	if (mock.zero_write) return 0;
	if (mock.write_limit && size > (unsigned int)mock.write_limit)
		size = (SceSize)mock.write_limit;
	assert(mock.written + size < sizeof(mock.created));
	memcpy(mock.created + mock.written, data, size);
	mock.written += size;
	mock.created[mock.written] = 0;
	return (int)size;
}
int ksceIoClose(SceUID fd)
{
	if (fd == 7) mock.closes++;
	else {assert(fd == 8); mock.create_closes++;}
	return 0;
}
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

static void test_config_creation(void)
{
	vitastream_settings_load();
	assert(vitastream_settings_speakers_on());
	assert(!vitastream_settings_lowmemory_on());
	assert(!strcmp(mock.created, "screen=on\nspeakers=on\nlowmemory=off\nnosleep=on\n"));
	assert(mock.creates == 1 && mock.writes == 1 && mock.create_closes == 1);
	assert(!mock.reads && !mock.closes);
	vitastream_settings_load();
	assert(mock.creates == 1 && mock.writes == 1 && mock.reads == 1 && mock.closes == 1);

	memset(&mock, 0, sizeof(mock));
	mock.write_limit = 3;
	vitastream_settings_load();
	assert(!strcmp(mock.created, "screen=on\nspeakers=on\nlowmemory=off\nnosleep=on\n"));
	assert(mock.writes > 1 && mock.create_closes == 1);

	memset(&mock, 0, sizeof(mock));
	mock.fail_create = 1;
	vitastream_settings_load();
	assert(vitastream_settings_speakers_on() && mock.creates == 1);
	assert(!mock.writes && !mock.create_closes);

	memset(&mock, 0, sizeof(mock));
	mock.fail_write = 1;
	vitastream_settings_load();
	assert(vitastream_settings_speakers_on() && mock.writes == 1 && mock.create_closes == 1);

	memset(&mock, 0, sizeof(mock));
	mock.zero_write = 1;
	vitastream_settings_load();
	assert(vitastream_settings_speakers_on() && mock.writes == 1 && mock.create_closes == 1);

	memset(&mock, 0, sizeof(mock));
	mock.text = "screen=off\nspeakers=off\n";
	vitastream_settings_load();
	assert(!vitastream_settings_speakers_on() && !mock.creates && !mock.writes);
	assert(!strcmp(mock.text, "screen=off\nspeakers=off\n"));
	assert(!vitastream_settings_lowmemory_on()); /* older config retains default */
	mock.text = "screen=off\nspeakers=off\nlowmemory=on\n";
	vitastream_settings_load();
	assert(vitastream_settings_lowmemory_on() && !mock.creates && !mock.writes);
	mock.text = "screen=off\nspeakers=off\n";
	mock.fail_read = 1;
	vitastream_settings_load();
	assert(vitastream_settings_speakers_on() && !mock.creates && !mock.writes);
	assert(!vitastream_settings_lowmemory_on());
	mock.fail_open = 1;
	vitastream_settings_load();
	assert(vitastream_settings_speakers_on() && mock.creates == 1 && !mock.writes);
	assert(!strcmp(mock.text, "screen=off\nspeakers=off\n"));

	memset(&mock, 0, sizeof(mock));
	mock.race_text = "screen=off\nspeakers=off\n";
	vitastream_settings_load();
	assert(mock.creates == 1 && !mock.writes && !mock.create_closes);
	assert(mock.text == mock.race_text);
	vitastream_settings_load();
	assert(!vitastream_settings_speakers_on() && mock.creates == 1);
	memset(&mock, 0, sizeof(mock));
}

static void test_nosleep_policy(void)
{
	memset(&mock, 0, sizeof(mock));
	mock.text = "screen=on\nspeakers=on"; /* older files default to nosleep=on */
	vitastream_settings_load();
	assert(vitastream_nosleep_update(0) == 0 && !mock.power_ticks);
	assert(vitastream_nosleep_update(1) == 0 && mock.power_ticks == 1);
	mock.time = 999999;
	assert(vitastream_nosleep_update(1) == 0 && mock.power_ticks == 1);
	mock.time = 1000000;
	assert(vitastream_nosleep_update(1) == 0 && mock.power_ticks == 2);
	assert(vitastream_nosleep_update(0) == 0);
	mock.time += 5000000;
	assert(vitastream_nosleep_update(0) == 0 && mock.power_ticks == 2);
	assert(vitastream_nosleep_update(1) == 0 && mock.power_ticks == 3);
	mock.text = "nosleep=off";
	vitastream_settings_load(); mock.time += 5000000;
	assert(vitastream_nosleep_update(1) == 0 && mock.power_ticks == 3);
	mock.text = "nosleep=on";
	vitastream_settings_load(); mock.power_result = -81;
	assert(vitastream_nosleep_update(1) == -81 && mock.power_ticks == 4);
	assert(vitastream_nosleep_update(1) == -81 && mock.power_ticks == 4);
	mock.power_result = 0; mock.time += 1000000;
	assert(vitastream_nosleep_update(1) == 0 && mock.power_ticks == 5);
	assert(vitastream_nosleep_update(0) == 0);
	memset(&mock, 0, sizeof(mock));
}

int main(void)
{
	test_config_creation();
	test_nosleep_policy();
	vitastream_settings_load();
	assert(vitastream_settings_speakers_on() && !mock.reads && !mock.closes);
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
	assert(vitastream_settings_speakers_on());
	assert(vitastream_screen_update(1) == 0 && mock.off == 2);
	mock.fail_read = 1;
	vitastream_settings_load();
	assert(vitastream_settings_speakers_on() && mock.reads == mock.closes);
	puts("settings: automatic config creation, preserved files, I/O failures, panel policy and restoration retries passed");
	return 0;
}
