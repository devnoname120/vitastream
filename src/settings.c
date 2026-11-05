#include <psp2kern/io/fcntl.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/types.h>
#include <taihen.h>
#include <stdint.h>
#include "config.h"
#include "settings.h"
#include "diagnostic.h"

int module_get_export_func(SceUID pid, const char *module_name,
	uint32_t library_nid, uint32_t function_nid, uintptr_t *function);

struct panel_api {
	int (*on)(void);
	int (*off)(void);
	int (*brightness)(void);
	int (*set_brightness)(int value);
};

static struct vitastream_config config = {1, 1, 0};
static struct panel_api panel;
static int panel_resolved;
static int panel_owned;
static int saved_brightness;
static int panel_is_oled;
static int panel_on_requested;

static int resolve_panel(const char *module, uint32_t library,
	const uint32_t nids[4], struct panel_api *api)
{
	uintptr_t functions[4];
	for (unsigned int i = 0; i < 4; i++) {
		int ret = module_get_export_func(KERNEL_PID, module, library,
			nids[i], &functions[i]);
		if (ret < 0 || !functions[i])
			return ret < 0 ? ret : -1;
	}
	api->on = (int (*)(void))functions[0];
	api->off = (int (*)(void))functions[1];
	api->brightness = (int (*)(void))functions[2];
	api->set_brightness = (int (*)(int))functions[3];
	return 0;
}

static int resolve_active_panel(void)
{
	static const uint32_t oled_nids[] = {0x4C7836C7, 0xBC84602E, 0x43EF811A, 0xF9624C47};
	static const uint32_t lcd_nids[] = {0x5F4124AB, 0x1A0A7519, 0x3A6D6AC3, 0x581D3A87};
	struct panel_api oled, lcd;
	int has_oled = resolve_panel("SceOled", 0x60C7478A, oled_nids, &oled) >= 0;
	int has_lcd = resolve_panel("SceLcd", 0xFA916D71, lcd_nids, &lcd) >= 0;
	/* Never call one panel's driver on another model, including a TV. */
	if (has_oled == has_lcd)
		return -1;
	panel = has_oled ? oled : lcd;
	panel_is_oled = has_oled;
	return 0;
}

static void create_default_config(void)
{
	static const char text[] = "screen=on\nspeakers=on\nlowmemory=off\n";
	size_t length = sizeof(text) - 1, written = 0;
	/* An unsuccessful read must never truncate an existing configuration,
	 * including one created between the read and this exclusive open. */
	SceUID fd = ksceIoOpen(VITASTREAM_CONFIG_PATH,
		SCE_O_WRONLY | SCE_O_CREAT | SCE_O_EXCL, 0666);
	if (fd < 0) {
		diagnostic_record("create default vitastream.cfg", fd);
		return;
	}
	int ret = 0;
	while (written < length) {
		ret = ksceIoWrite(fd, text + written, length - written);
		if (ret <= 0) {
			if (!ret)
				ret = -1;
			break;
		}
		written += (size_t)ret;
	}
	int close_ret = ksceIoClose(fd);
	diagnostic_record("create default vitastream.cfg", ret < 0 ? ret : close_ret);
}

void vitastream_settings_load(void)
{
	static char text[VITASTREAM_CONFIG_MAX_SIZE + 1];
	struct vitastream_config parsed;
	SceUID fd;
	int ret;
	vitastream_config_defaults(&config);
	fd = ksceIoOpen(VITASTREAM_CONFIG_PATH, SCE_O_RDONLY, 0);
	if (fd >= 0) {
		ret = ksceIoRead(fd, text, sizeof(text));
		ksceIoClose(fd);
		if (ret >= 0 && vitastream_config_parse(text, (size_t)ret, &parsed) == 0)
			config = parsed;
		else
			diagnostic_record("invalid vitastream.cfg; using defaults", ret < 0 ? ret : -1);
	} else {
		create_default_config();
	}
	diagnostic_record("config screen on", config.screen_on);
	diagnostic_record("config local speakers on", config.speakers_on);
	diagnostic_record("config lowmemory on", config.lowmemory_on);
}

int vitastream_settings_speakers_on(void)
{
	return config.speakers_on;
}

int vitastream_settings_lowmemory_on(void)
{
	return config.lowmemory_on;
}

int vitastream_screen_restore(void)
{
	int ret;
	if (!panel_owned)
		return 0;
	/* OLED SetBrightness restores gamma and appends its own display-on
	 * sequence after a zero-to-nonzero transition. A separate DisplayOn
	 * would leave a pending command and make brightness restoration busy. */
	if (!panel_is_oled && !panel_on_requested) {
		ret = panel.on();
		if (ret < 0)
			return ret;
		panel_on_requested = 1;
	}
	for (unsigned int attempt = 0; ; attempt++) {
		ret = panel.set_brightness(saved_brightness);
		if (!panel_is_oled || ret != (int)0x803F0A05 || attempt == 49)
			break;
		/* The earlier off sequence may still be in flight. This runs
		 * on a worker, never in a USB callback or interrupt handler. */
		ksceKernelDelayThread(10000);
	}
	if (ret < 0)
		return ret;
	panel_owned = 0;
	panel_on_requested = 0;
	diagnostic_record("restore local display", 0);
	return 0;
}

int vitastream_screen_update(int active)
{
	int ret;
	if (!active || config.screen_on)
		return vitastream_screen_restore();
	if (panel_owned)
		return 0;
	if (!panel_resolved) {
		ret = resolve_active_panel();
		if (ret < 0)
			return ret;
		panel_resolved = 1;
	}
	saved_brightness = panel.brightness();
	if (saved_brightness < 0)
		return saved_brightness;
	ret = panel.off();
	if (ret < 0)
		return ret;
	panel_owned = 1;
	panel_on_requested = 0;
	diagnostic_record("disable local display for capture", 0);
	return 0;
}
