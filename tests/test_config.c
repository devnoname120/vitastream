#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "config.h"

static void parse(const char *text, int screen, int speakers)
{
	struct vitastream_config config = {7, 9};
	assert(vitastream_config_parse(text, strlen(text), &config) == 0);
	assert(config.screen_on == screen && config.speakers_on == speakers);
}

int main(void)
{
	parse("", 1, 0);
	parse("screen=on\nspeakers=on\n", 1, 1);
	parse("screen=off\nspeakers=on", 0, 1);
	parse("screen=on\nspeakers=off\n", 1, 0);
	parse("screen=off\nspeakers=off\n", 0, 0);
	parse(" # header\r\n ScReEn = OFF ; comment\r\n\tspeakers = ON  # note\n", 0, 1);
	parse("speakers=on\nspeakers=off\nscreen=off\nscreen=on", 1, 0);
	parse("\xEF\xBB\xBFscreen=off\nspeakers=on", 0, 1);
	const char *invalid[] = {"screen", "screen=", "screen=yes", "screen=on=off",
		"speaker=on", "micro=on", "screen=off\nspeakers=invalid", "=on"};
	for (unsigned int i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		struct vitastream_config config = {7, 9};
		assert(vitastream_config_parse(invalid[i], strlen(invalid[i]), &config) < 0);
		assert(config.screen_on == 7 && config.speakers_on == 9);
	}
	char data[VITASTREAM_CONFIG_MAX_SIZE + 1];
	memset(data, ' ', sizeof(data));
	struct vitastream_config config = {7, 9};
	assert(vitastream_config_parse(data, VITASTREAM_CONFIG_MAX_SIZE, &config) == 0);
	assert(vitastream_config_parse(data, sizeof(data), &config) < 0);
	data[10] = 0;
	assert(vitastream_config_parse(data, 20, &config) < 0);
	assert(vitastream_config_parse(NULL, 0, &config) < 0);
	assert(vitastream_config_parse("", 0, NULL) < 0);
	puts("config: four policies, defaults, comments/CRLF, duplicates, size limits and atomic rejection passed");
	return 0;
}
