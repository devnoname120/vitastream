#include <psp2kern/udcd.h>
#include <taihen.h>

#include "diagnostic.h"
#include "firmware.h"
#include "firmware_profiles.h"

int module_get_offset(SceUID pid, SceUID modid, int segment_index,
	size_t offset, uintptr_t *address);

static int resolve_module(enum FirmwareModule module, SceUID *modid,
	uintptr_t *data_address)
{
	static const char *const names[] = {
		"SceUdcd", "SceAudio", "SceKernelDmacMgr"
	};
	tai_module_info_t identity = {0};
	uintptr_t text, text_end, data, data_end;
	size_t text_size, data_size;
	int ret;

	identity.size = sizeof(identity);
	ret = taiGetModuleInfoForKernel(KERNEL_PID, names[module], &identity);
	if (ret < 0)
		return ret;
	diagnostic_record(names[module], (int)identity.module_nid);
	/* Do not inspect code/data from an unknown module revision. */
	if (!firmware_required_sizes(module, identity.module_nid, &text_size, &data_size))
		return SCE_UDCD_ERROR_INVALID_ARGUMENT;

	ret = module_get_offset(KERNEL_PID, identity.modid, 0, 0, &text);
	if (ret < 0)
		return ret;
	/* taiHEN accepts the exclusive segment end. Asking for the full size,
	 * rather than size-1, rejects a segment one byte shorter than required. */
	ret = module_get_offset(KERNEL_PID, identity.modid, 0, text_size, &text_end);
	if (ret < 0)
		return ret;
	ret = module_get_offset(KERNEL_PID, identity.modid, 1, 0, &data);
	if (ret < 0)
		return ret;
	ret = module_get_offset(KERNEL_PID, identity.modid, 1, data_size, &data_end);
	if (ret < 0)
		return ret;
	if (!text || !data || (text & 3u) || (data & 3u) ||
	    text_end < text || text_end - text != text_size ||
	    data_end < data || data_end - data != data_size ||
	    !firmware_module_matches(module, identity.module_nid,
		(const unsigned char *)text, text_size, data_size))
		return SCE_UDCD_ERROR_INVALID_ARGUMENT;

	if (modid)
		*modid = identity.modid;
	if (data_address)
		*data_address = data;
	return 0;
}

int firmware_prepare(SceUID *udcd_modid)
{
	SceUID validated_id;
	int ret;
	if (!udcd_modid)
		return SCE_UDCD_ERROR_INVALID_ARGUMENT;
	ret = resolve_module(FIRMWARE_UDCD, &validated_id, NULL);
	if (ret < 0)
		return ret;
	ret = resolve_module(FIRMWARE_AUDIO, NULL, NULL);
	if (ret < 0)
		return ret;
	ret = resolve_module(FIRMWARE_DMAC, NULL, NULL);
	if (ret < 0)
		return ret;
	*udcd_modid = validated_id;
	return 0;
}

int firmware_audio_data(uintptr_t *data_address)
{
	if (!data_address)
		return SCE_UDCD_ERROR_INVALID_ARGUMENT;
	return resolve_module(FIRMWARE_AUDIO, NULL, data_address);
}
