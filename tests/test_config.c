#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "config.h"

static void parse_all(const char *text, int screen, int speakers, int lowmemory)
{
	struct vitastream_config config = {7, 9, 11};
	assert(vitastream_config_parse(text, strlen(text), &config) == 0);
	assert(config.screen_on == screen && config.speakers_on == speakers);
	assert(config.lowmemory_on == lowmemory);
}

static void parse(const char *text, int screen, int speakers)
{
	parse_all(text, screen, speakers, 0);
}

int main(void)
{
	parse("", 1, 1);
	parse("# defaults\n", 1, 1);
	parse("screen=off\n", 0, 1);
	parse("speakers=off\n", 1, 0);
	parse("screen=on\nspeakers=on\n", 1, 1);
	parse("screen=off\nspeakers=on", 0, 1);
	parse("screen=on\nspeakers=off\n", 1, 0);
	parse("screen=off\nspeakers=off\n", 0, 0);
	parse(" # header\r\n ScReEn = OFF ; comment\r\n\tspeakers = ON  # note\n", 0, 1);
	parse("speakers=on\nspeakers=off\nscreen=off\nscreen=on", 1, 0);
	parse("\xEF\xBB\xBFscreen=off\nspeakers=on", 0, 1);
	parse_all("lowmemory=on", 1, 1, 1);
	parse_all("screen=off\nspeakers=off\nlowmemory=off", 0, 0, 0);
	parse_all(" LoWmEmOrY = ON ; serial capture\r\n", 1, 1, 1);
	parse_all("lowmemory=1", 1, 1, 1);
	parse_all("lowmemory=0", 1, 1, 0);
	parse_all("lowmemory=on\nlowmemory=off", 1, 1, 0);
	const char *invalid[] = {"screen", "screen=", "screen=yes", "screen=on=off",
		"speaker=on", "micro=on", "screen=off\nspeakers=invalid", "=on",
		"lowmemory=2", "lowmemory=yes", "lowmemory=on\nscreen=broken"};
	for (unsigned int i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		struct vitastream_config config = {7, 9, 11};
		assert(vitastream_config_parse(invalid[i], strlen(invalid[i]), &config) < 0);
		assert(config.screen_on == 7 && config.speakers_on == 9);
		assert(config.lowmemory_on == 11);
	}
	char data[VITASTREAM_CONFIG_MAX_SIZE + 1];
	memset(data, ' ', sizeof(data));
	struct vitastream_config config = {7, 9, 11};
	assert(vitastream_config_parse(data, VITASTREAM_CONFIG_MAX_SIZE, &config) == 0);
	assert(vitastream_config_parse(data, sizeof(data), &config) < 0);
	data[10] = 0;
	assert(vitastream_config_parse(data, 20, &config) < 0);
	assert(vitastream_config_parse(NULL, 0, &config) < 0);
	assert(vitastream_config_parse("", 0, NULL) < 0);
	puts("config: screen, speakers, lowmemory, defaults and atomic parsing passed");
	return 0;
}
