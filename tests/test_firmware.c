#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <taihen.h>
#include "firmware.h"
#include "firmware_profiles.h"

static unsigned char text[3][0xD200] __attribute__((aligned(16)));
static unsigned char data[3][0xBD24] __attribute__((aligned(16)));
static uint32_t nids[3];
static struct {
	struct {void *vaddr; size_t memsz;} segments[2];
} modules[3];
static int fail_lookup, fail_info, fail_at_call, info_calls;

int taiGetModuleInfoForKernel(SceUID pid, const char *name, tai_module_info_t *info)
{
	static const char *const names[] = {"SceUdcd", "SceAudio", "SceKernelDmacMgr"};
	assert(pid == KERNEL_PID && info->size == sizeof(*info));
	for (int i = 0; i < 3; i++) {
		if (strcmp(name, names[i]))
			continue;
		if (i == fail_lookup)
			return -77;
		info->modid = i;
		info->module_nid = nids[i];
		return 0;
	}
	assert(0);
	return -1;
}

int module_get_offset(SceUID pid, SceUID modid, int segment_index,
	size_t offset, uintptr_t *address)
{
	assert(pid == KERNEL_PID && modid >= 0 && modid < 3);
	assert(segment_index == 0 || segment_index == 1);
	info_calls++;
	if (info_calls == fail_at_call)
		return -80;
	if (modid == fail_info)
		return -78;
	if (offset > modules[modid].segments[segment_index].memsz)
		return -79;
	*address = (uintptr_t)modules[modid].segments[segment_index].vaddr + offset;
	return 0;
}

static void reset(unsigned int revision)
{
	fail_lookup = fail_info = -1;
	fail_at_call = 0;
	info_calls = 0;
	nids[0] = g_udcd_nids[revision];
	nids[1] = g_audio_nids[revision];
	nids[2] = g_dmac_nids[revision];
	memset(text, 0, sizeof(text));
	memcpy(text[0] + FIRMWARE_UDCD_CONFIG_OFFSET,
		g_udcd_config_context, sizeof(g_udcd_config_context));
	memcpy(text[1] + 0x35C4, g_audio_capture_context, sizeof(g_audio_capture_context));
	memcpy(text[1] + 0x7FD8, g_i2s_register_phys, sizeof(g_i2s_register_phys));
	memcpy(text[2] + 0x5F0, g_dmac_callback_context, sizeof(g_dmac_callback_context));
	memcpy(text[2] + 0x1DA4, g_dmac_cpu_context, sizeof(g_dmac_cpu_context));
	for (int i = 0; i < 3; i++) {
		modules[i].segments[0].vaddr = text[i];
		modules[i].segments[0].memsz = sizeof(text[i]);
		modules[i].segments[1].vaddr = data[i];
		modules[i].segments[1].memsz = sizeof(data[i]);
	}
}

static void reject(void)
{
	SceUID id = -99;
	assert(firmware_prepare(&id) < 0);
	assert(id == -99);
}

int main(void)
{
	SceUID id;
	uintptr_t address;
	for (unsigned int revision = 0; revision < 12; revision++) {
		reset(revision);
		assert(firmware_prepare(&id) == 0 && id == 0 && info_calls == 12);
		assert(firmware_audio_data(&address) == 0 && address == (uintptr_t)data[1]);
		for (int module = 0; module < 3; module++) {
			size_t required_text, required_data;
			assert(firmware_required_sizes(module, nids[module], &required_text, &required_data));
			reset(revision);
			nids[module] = 0xFFFFFFFF;
			reject();
			assert(info_calls == 4 * module);
			reset(revision); fail_lookup = module; reject();
			reset(revision); fail_info = module; reject();
			reset(revision); modules[module].segments[0].vaddr = NULL; reject();
			reset(revision); modules[module].segments[1].vaddr = NULL; reject();
			reset(revision); modules[module].segments[0].vaddr = text[module] + 1; reject();
			reset(revision); modules[module].segments[0].memsz = 1; reject();
			reset(revision); modules[module].segments[1].memsz = 1; reject();
			reset(revision); modules[module].segments[0].memsz = required_text - 1; reject();
			reset(revision); modules[module].segments[1].memsz = required_data - 1; reject();
			reset(revision); modules[module].segments[1].vaddr = data[module] + 1; reject();
			reset(revision);
			text[module][module == 0 ? 0x128C : module == 1 ? 0x35C4 : 0x5F0] ^= 1;
			reject();
		}
		reset(revision); text[2][0x1DA4] ^= 1; reject();
		reset(revision);
		nids[1] = 0;
		address = 123;
		assert(firmware_audio_data(&address) < 0 && address == 123);
		for (int call = 1; call <= 12; call++) {
			reset(revision); fail_at_call = call; reject();
		}
	}
	assert(firmware_prepare(NULL) < 0 && firmware_audio_data(NULL) < 0);
	reset(3);
	nids[0] = 0xF5586BE8;
	nids[1] = 0x0086EFAD;
	nids[2] = 0x9480785B;
	assert(firmware_prepare(&id) == 0);
	for (unsigned int i = 0; i < 16; i++) {
		uint32_t destination = i < 8 ? g_i2s_register_phys[i] + 0x100 : 0;
		int expected = i == 3 || i == 4 || i == 5 || i == 7;
		assert(firmware_audio_sink_matches(destination, i, 0xFF) == expected);
		assert(!firmware_audio_sink_matches(destination, i, 0));
		assert(!firmware_audio_sink_matches(destination + 4, i, 0xFF));
	}
	assert(!firmware_audio_sink_matches(0, (unsigned int)-1, 0xFF));
	reset(0); text[1][0x7FD8] ^= 1; reject();
	assert(!firmware_module_matches((enum FirmwareModule)99, 0, NULL, 0, 0));
	assert(!firmware_context_matches(text[0], 8, (size_t)-1, text[0], 4));
	puts("firmware: 12 revisions and lookup, bounds, identity, context, ABI failure scenarios passed");
	return 0;
}
