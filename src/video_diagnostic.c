#include <stdatomic.h>
#include <string.h>
#include "diagnostic.h"
#include "video_diagnostic.h"

#define CONTROL_RECORDS 32u
#define FAILURE_LIMIT 32u

struct control_record {
	unsigned int event, format, frame, interval, width, height;
};
static struct control_record controls[CONTROL_RECORDS];
static atomic_uint control_read, control_write, control_dropped;
static atomic_flag control_producer = ATOMIC_FLAG_INIT;
static unsigned int frame_attempt, failures, seen_failures;
static int last_result[VIDEO_DIAG_STAGE_COUNT];
static unsigned int mode[5];
static int mode_valid, source_valid;
static struct {
	int head, pid;
	uintptr_t base, physical;
	unsigned int width, height, pitch, format;
} source;

_Static_assert(VIDEO_DIAG_STAGE_COUNT <= 32, "failure mask too small");

void video_diag_control(enum video_diag_control event, unsigned int format,
	unsigned int frame, unsigned int interval, unsigned int width,
	unsigned int height)
{
	/* Never spin or wait on a callback's producer. A missing record is
	 * explicit in the worker log rather than silently inventing history. */
	if (atomic_flag_test_and_set_explicit(&control_producer, memory_order_acquire)) {
		atomic_fetch_add_explicit(&control_dropped, 1, memory_order_relaxed);
		return;
	}
	unsigned int write = atomic_load_explicit(&control_write, memory_order_relaxed);
	unsigned int read = atomic_load_explicit(&control_read, memory_order_acquire);
	if (write - read == CONTROL_RECORDS) {
		atomic_fetch_add_explicit(&control_dropped, 1, memory_order_relaxed);
	} else {
		controls[write % CONTROL_RECORDS] = (struct control_record){
			event, format, frame, interval, width, height
		};
		atomic_store_explicit(&control_write, write + 1, memory_order_release);
	}
	atomic_flag_clear_explicit(&control_producer, memory_order_release);
}

void video_diag_system_event(int resume, int event, int state)
{
	video_diag_control(VIDEO_DIAG_SYSTEM_EVENT, (unsigned int)resume,
		(unsigned int)event, (unsigned int)state, 0, 0);
}

void video_diag_system_wait_result(int result)
{
	video_diag_control(VIDEO_DIAG_SYSTEM_WAIT, (unsigned int)result, 0, 0, 0, 0);
}

void video_diag_flush(void)
{
	static const char *const labels[][6] = {
		{"UVC RESET event", "UVC RESET format", "UVC RESET frame", "UVC RESET interval", "UVC RESET width", "UVC RESET height"},
		{"UVC PROBE event", "UVC PROBE format", "UVC PROBE frame", "UVC PROBE interval", "UVC PROBE width", "UVC PROBE height"},
		{"UVC COMMIT event", "UVC COMMIT format", "UVC COMMIT frame", "UVC COMMIT interval", "UVC COMMIT width", "UVC COMMIT height"}
	};
	for (unsigned int n = 0; n < CONTROL_RECORDS; n++) {
		unsigned int read = atomic_load_explicit(&control_read, memory_order_relaxed);
		if (read == atomic_load_explicit(&control_write, memory_order_acquire))
			break;
		struct control_record record = controls[read % CONTROL_RECORDS];
		atomic_store_explicit(&control_read, read + 1, memory_order_release);
		if (record.event == VIDEO_DIAG_SYSTEM_EVENT) {
			diagnostic_record("system event direction", (int)record.format);
			diagnostic_record("system event id", (int)record.frame);
			diagnostic_record("system event capture state", (int)record.interval);
			continue;
		}
		if (record.event == VIDEO_DIAG_SYSTEM_WAIT) {
			diagnostic_record("system quiesce wait result", (int)record.format);
			continue;
		}
		if (record.event > VIDEO_DIAG_COMMIT)
			continue;
		if (record.event == VIDEO_DIAG_RESET) {
			frame_attempt = failures = seen_failures = 0;
			mode_valid = source_valid = 0;
		}
		const unsigned int values[] = {record.event, record.format,
			record.frame, record.interval, record.width, record.height};
		for (unsigned int i = 0; i < 6; i++)
			diagnostic_record(labels[record.event][i], (int)values[i]);
	}
	unsigned int dropped = atomic_exchange_explicit(&control_dropped, 0, memory_order_relaxed);
	if (dropped)
		diagnostic_record("UVC control diagnostic records dropped", (int)dropped);
}

void video_diag_mode(unsigned int format, unsigned int frame,
	unsigned int interval, unsigned int width, unsigned int height)
{
	const unsigned int current[] = {format, frame, interval, width, height};
	static const char *const labels[] = {"UVC used format", "UVC used frame",
		"UVC used interval", "UVC used width", "UVC used height"};
	frame_attempt++;
	if (mode_valid && !memcmp(mode, current, sizeof(mode)))
		return;
	memcpy(mode, current, sizeof(mode));
	mode_valid = 1;
	seen_failures = 0;
	for (unsigned int i = 0; i < 5; i++)
		diagnostic_record(labels[i], (int)mode[i]);
	diagnostic_record("UVC used NV12 payload bytes", (int)(width * height * 3u / 2u));
}

void video_diag_source(int head, int pid, uintptr_t base, uintptr_t physical,
	unsigned int width, unsigned int height, unsigned int pitch,
	unsigned int format)
{
	/* Ordinary double-buffer swaps must not cause per-frame file writes. */
	int changed = !source_valid || source.head != head || source.pid != pid ||
		source.width != width || source.height != height ||
		source.pitch != pitch || source.format != format;
	source.head = head; source.pid = pid;
	source.base = base; source.physical = physical;
	source.width = width; source.height = height;
	source.pitch = pitch; source.format = format;
	source_valid = 1;
	if (!changed)
		return;
	seen_failures = 0;
	diagnostic_record("video source head", head);
	diagnostic_record("video source pid", pid);
	diagnostic_record("video source width", (int)width);
	diagnostic_record("video source height", (int)height);
	diagnostic_record("video source pitch", (int)pitch);
	diagnostic_record("video source pixel format", (int)format);
}

int video_diag_result(enum video_diag_stage stage, int result)
{
	static const char *const labels[VIDEO_DIAG_STAGE_COUNT] = {
		"video fail format selection", "video fail buffer prepare", "video fail framebuffer query",
		"video fail scan window", "video fail source geometry", "video fail process reference",
		"video fail source page lock", "video fail VA start", "video fail VA end",
		"video fail physical span", "video fail IFTU CSC", "video fail source unlock",
		"video fail process release", "video fail prior USB wait", "video fail USB submission",
		"video fail USB event wait", "video fail USB completion", "video fail allocation",
		"video fail buffer base", "video fail buffer physical", "video fail allocation release"
	};
	if (result >= 0 || stage < 0 || stage >= VIDEO_DIAG_STAGE_COUNT)
		return result;
	unsigned int bit = 1u << stage;
	if ((seen_failures & bit) && last_result[stage] == result)
		return result;
	seen_failures |= bit;
	last_result[stage] = result;
	if (failures >= FAILURE_LIMIT)
		return result;
	failures++;
	diagnostic_record(labels[stage], result);
	diagnostic_record("video failed frame attempt", (int)frame_attempt);
	if (source_valid) {
		diagnostic_record("video last source pid", source.pid);
		diagnostic_record("video last source virtual", (int)source.base);
		diagnostic_record("video last source physical", (int)source.physical);
	}
	if (failures == FAILURE_LIMIT)
		diagnostic_record("video failure detail limit reached", FAILURE_LIMIT);
	return result;
}
