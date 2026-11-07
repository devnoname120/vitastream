#include "config.h"

static int space(char c)
{
	return c == ' ' || c == '\t' || c == '\r';
}

static int equal(const char *text, size_t length, const char *word)
{
	size_t i;
	for (i = 0; i < length && word[i]; i++) {
		char c = text[i];
		if (c >= 'A' && c <= 'Z')
			c += 'a' - 'A';
		if (c != word[i])
			return 0;
	}
	return i == length && !word[i];
}

void vitastream_config_defaults(struct vitastream_config *config)
{
	config->screen_on = 1;
	config->speakers_on = 1;
	config->lowmemory_on = 0;
	config->nosleep_on = 1;
}

int vitastream_config_parse(const char *text, size_t length,
	struct vitastream_config *config)
{
	struct vitastream_config parsed;
	size_t position = 0;
	if (!text || !config || length > VITASTREAM_CONFIG_MAX_SIZE)
		return -1;
	vitastream_config_defaults(&parsed);
	if (length >= 3 && (unsigned char)text[0] == 0xEF &&
	    (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
		position = 3;
	while (position < length) {
		size_t start = position, end, separator, key_end, value;
		int enabled;
		while (position < length && text[position] != '\n') {
			if (!text[position])
				return -1;
			position++;
		}
		end = position;
		if (position < length)
			position++;
		for (size_t i = start; i < end; i++) {
			if (text[i] == '#' || text[i] == ';') {
				end = i;
				break;
			}
		}
		while (start < end && space(text[start])) start++;
		while (end > start && space(text[end - 1])) end--;
		if (start == end)
			continue;
		separator = start;
		while (separator < end && text[separator] != '=') separator++;
		if (separator == end)
			return -1;
		key_end = separator;
		while (key_end > start && space(text[key_end - 1])) key_end--;
		value = separator + 1;
		while (value < end && space(text[value])) value++;
		int lowmemory = equal(text + start, key_end - start, "lowmemory");
		int nosleep = equal(text + start, key_end - start, "nosleep");
		if (equal(text + value, end - value, "on") ||
		    ((lowmemory || nosleep) && equal(text + value, end - value, "1"))) enabled = 1;
		else if (equal(text + value, end - value, "off") ||
		    ((lowmemory || nosleep) && equal(text + value, end - value, "0"))) enabled = 0;
		else return -1;
		if (equal(text + start, key_end - start, "screen"))
			parsed.screen_on = enabled;
		else if (equal(text + start, key_end - start, "speakers"))
			parsed.speakers_on = enabled;
		else if (lowmemory)
			parsed.lowmemory_on = enabled;
		else if (nosleep)
			parsed.nosleep_on = enabled;
		else
			return -1;
	}
	*config = parsed;
	return 0;
}
