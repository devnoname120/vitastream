#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psp2kern/io/fcntl.h>
#include "diagnostic.h"
#include "video_diagnostic.h"

static struct file {
	char text[65536];
	size_t length;
	int exists;
} files[2];
static int opens, writes, fail_rename;
static _Thread_local int callback_context;

static int file_index(const char *path)
{
	if (!strcmp(path, "ux0:data/vitastream_diag.txt")) return 0;
	assert(!strcmp(path, "ux0:data/vitastream_diag.previous.txt"));
	return 1;
}

SceUID ksceIoOpen(const char *path, int flags, int mode)
{
	(void)mode;
	assert(!callback_context);
	opens++;
	int index = file_index(path);
	if (!files[index].exists && !(flags & SCE_O_CREAT)) return -2;
	files[index].exists = 1;
	if (flags & SCE_O_TRUNC) {
		files[index].length = 0;
		files[index].text[0] = 0;
	}
	return index + 1;
}

int ksceIoWrite(SceUID fd, const void *data, SceSize size)
{
	assert(!callback_context && fd >= 1 && fd <= 2);
	struct file *file = &files[fd - 1];
	assert(file->length + size < sizeof(file->text));
	memcpy(file->text + file->length, data, size);
	file->length += size;
	file->text[file->length] = 0;
	writes++;
	return (int)size;
}

int ksceIoClose(SceUID fd) {assert(!callback_context && fd >= 1 && fd <= 2); return 0;}
int ksceIoRemove(const char *path) {memset(&files[file_index(path)], 0, sizeof(files[0])); return 0;}
int ksceIoRename(const char *old_path, const char *new_path)
{
	assert(!callback_context);
	if (fail_rename) return -7;
	int old_index = file_index(old_path), new_index = file_index(new_path);
	files[new_index] = files[old_index];
	memset(&files[old_index], 0, sizeof(files[old_index]));
	return 0;
}

static unsigned int occurrences(const char *needle)
{
	unsigned int count = 0;
	const char *where = files[0].text;
	while ((where = strstr(where, needle)) != NULL) {count++; where += strlen(needle);}
	return count;
}

static void *produce(void *arg)
{
	unsigned int frame = (unsigned int)(uintptr_t)arg;
	callback_context = 1;
	for (unsigned int i = 0; i < 20; i++)
		video_diag_control(VIDEO_DIAG_PROBE, 1, frame, 333333, 480, 272);
	callback_context = 0;
	return NULL;
}

int main(void)
{
	files[0].exists = 1;
	strcpy(files[0].text, "failed boot evidence\n");
	files[0].length = strlen(files[0].text);
	diagnostic_reset();
	assert(!strcmp(files[1].text, "failed boot evidence\n"));
	assert(!strcmp(files[0].text, "vitastream boot diagnostic\nvideo diagnostic schema 0x00000001\n"));
	fail_rename = 1;
	diagnostic_record("retained", 123);
	diagnostic_reset();
	assert(strstr(files[0].text, "retained 0x0000007B\n"));
	assert(occurrences("vitastream boot diagnostic\n") == 2);
	fail_rename = 0;
	diagnostic_reset();

	int before = opens;
	callback_context = 1;
	video_diag_control(VIDEO_DIAG_RESET, 1, 1, 166666, 960, 544);
	video_diag_control(VIDEO_DIAG_PROBE, 1, 5, 333333, 1280, 720);
	video_diag_control(VIDEO_DIAG_COMMIT, 1, 4, 333333, 480, 272);
	callback_context = 0;
	assert(opens == before); /* no callback filesystem work */
	video_diag_flush();
	assert(strstr(files[0].text, "UVC PROBE width 0x00000500\n"));
	assert(strstr(files[0].text, "UVC COMMIT width 0x000001E0\n"));
	assert(strstr(files[0].text, "UVC PROBE event") < strstr(files[0].text, "UVC COMMIT event"));
	/* Distinguish the mode actually used from the received host COMMIT. */
	video_diag_mode(1, 5, 333333, 1280, 720);
	assert(strstr(files[0].text, "UVC used width 0x00000500\n"));
	video_diag_source(2, 123, 0x98000000, 0x20000000, 960, 544, 1024, 0);
	before = writes;
	for (unsigned int i = 0; i < 1000; i++) {
		video_diag_mode(1, 5, 333333, 1280, 720);
		video_diag_source(2, 123, 0x98000000 + ((i & 1) * 4096),
			0x20000000 + ((i & 1) * 4096), 960, 544, 1024, 0);
	}
	assert(writes == before); /* no per-frame writes or swap-address spam */
	assert(video_diag_result(VIDEO_DIAG_IFTU, -6) == -6);
	assert(strstr(files[0].text, "video fail IFTU CSC 0xFFFFFFFA\n"));
	assert(strstr(files[0].text, "video last source physical 0x20001000\n"));
	before = writes;
	for (unsigned int i = 0; i < 1000; i++) video_diag_result(VIDEO_DIAG_IFTU, -6);
	assert(writes == before);
	for (unsigned int i = 0; i < 100; i++)
		video_diag_result(VIDEO_DIAG_USB_COMPLETION, -100 - (int)i);
	assert(occurrences("video fail ") == 32);
	assert(occurrences("video failure detail limit reached") == 1);

	before = opens;
	pthread_t producers[8];
	for (uintptr_t i = 0; i < 8; i++) assert(!pthread_create(&producers[i], NULL, produce, (void *)(i + 1)));
	for (unsigned int i = 0; i < 8; i++) assert(!pthread_join(producers[i], NULL));
	assert(opens == before);
	video_diag_flush();
	assert(strstr(files[0].text, "UVC control diagnostic records dropped"));
	before = writes;
	video_diag_flush();
	assert(writes == before);
	before = opens;
	callback_context = 1;
	video_diag_system_event(0, 0x100, 1);
	video_diag_system_event(1, 0x400000, 2);
	video_diag_system_wait_result((int)0x80028004);
	callback_context = 0;
	assert(opens == before);
	video_diag_flush();
	assert(strstr(files[0].text, "system event id 0x00000100\n"));
	assert(strstr(files[0].text, "system event id 0x00400000\n"));
	assert(strstr(files[0].text, "system quiesce wait result 0x80028004\n"));
	puts("video diagnostics: deferred callbacks, mode mismatch, bounded errors, source swaps and boot-log retention passed");
	return 0;
}
