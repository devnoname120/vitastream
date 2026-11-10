#include <psp2kern/io/fcntl.h>
#include <string.h>

#include "diagnostic.h"

#define DIAGNOSTIC_FILE "ux0:data/vitastream_diag.txt"
#define DIAGNOSTIC_PREVIOUS_FILE "ux0:data/vitastream_diag.previous.txt"

static void diagnostic_write(const char *buffer, unsigned int length, int flags)
{
	SceUID fd;

	fd = ksceIoOpen(DIAGNOSTIC_FILE,
			SCE_O_WRONLY | SCE_O_CREAT | flags, 6);
	if (fd < 0)
		return;

	ksceIoWrite(fd, buffer, length);
	ksceIoClose(fd);
}

void diagnostic_reset(void)
{
	static const char header[] = "vitastream boot diagnostic\nvideo diagnostic schema 0x00000001\n";
	SceUID old = ksceIoOpen(DIAGNOSTIC_FILE, SCE_O_RDONLY, 0);
	int flags = SCE_O_APPEND;
	if (old >= 0) {
		ksceIoClose(old);
		(void)ksceIoRemove(DIAGNOSTIC_PREVIOUS_FILE);
		/* A reboot must not silently truncate the failed boot's evidence.
		 * If rotation fails, append a new boot header to the retained file. */
		if (ksceIoRename(DIAGNOSTIC_FILE, DIAGNOSTIC_PREVIOUS_FILE) >= 0)
			flags = SCE_O_TRUNC;
	}

	diagnostic_write(header, sizeof(header) - 1, flags);
}

void diagnostic_record(const char *stage, int result)
{
	static const char digits[] = "0123456789ABCDEF";
	char line[128];
	unsigned int length = 0;
	unsigned int value = (unsigned int)result;
	int shift;

	while (stage[length] && length < sizeof(line) - 15) {
		line[length] = stage[length];
		length++;
	}

	line[length++] = ' ';
	line[length++] = '0';
	line[length++] = 'x';
	for (shift = 28; shift >= 0; shift -= 4)
		line[length++] = digits[(value >> shift) & 0xF];
	line[length++] = '\n';

	diagnostic_write(line, length, SCE_O_APPEND);
}
