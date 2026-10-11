#ifndef VITASTREAM_FIRMWARE_PROFILES_H
#define VITASTREAM_FIRMWARE_PROFILES_H

#include <stddef.h>
#include <stdint.h>

enum FirmwareModule {
	FIRMWARE_UDCD,
	FIRMWARE_AUDIO,
	FIRMWARE_DMAC
};

#define FIRMWARE_UDCD_CONFIG_OFFSET 0x128Cu
#define FIRMWARE_AUDIO_DATA_SIZE 0xBD24u

/* External 3.60, 3.61, 3.63, 3.65, 3.67, 3.68, 3.69, 3.70,
 * 3.71, 3.72, 3.73 and 3.74, in that order. Available kit modules
 * have the same NIDs, code and segment geometry as their retail peers. */
static const uint32_t g_udcd_nids[] = {
	0x8E26B2A3u, 0x2BE21912u, 0x98D172C4u, 0xB0275A37u,
	0xCD08069Eu, 0xBB62E35Eu, 0xBFDD4BFDu, 0x9C6AA7D0u,
	0x3F5D04D9u, 0x2B7A20EFu, 0xBD25A100u, 0x7C44A78Bu
};
static const uint32_t g_audio_nids[] = {
	0x0DA7714Au, 0x5FA65AB0u, 0xB18B03DEu, 0xBA583E43u,
	0xFE1A2BC1u, 0x596C0CAAu, 0xCDD1D342u, 0x914AA642u,
	0x9439ED79u, 0xDEFAF2FBu, 0x0802F6F9u, 0x0BF11C75u
};
static const uint32_t g_dmac_nids[] = {
	0xF926C804u, 0x978FC8D7u, 0x85C581FEu, 0x01A3B7B0u,
	0x6B76C472u, 0x1FDCF223u, 0xEC00B329u, 0xBF27A234u,
	0x0858DD9Eu, 0xF5006200u, 0xC3261080u, 0x2D79F08Du
};

/* These contexts contain no absolute addresses or relocated immediates. */
static const unsigned char g_udcd_config_context[] = {
	0x2D,0xE9,0xF8,0x4F,0x81,0x46,0xD9,0xF8,
	0x04,0x20,0x01,0xF1,0x09,0x03,0x4F,0xF0,
	0x00,0x0A,0x4F,0xF6,0xFE,0x78,0x00,0x68,
	0x4A,0x60,0x08,0x60,0x99,0xF8,0x08,0x20,
	0x0A,0x72,0x99,0xF8,0x04,0x00,0xD9,0xF8,
	0x0C,0x70,0x18,0x37,0x00,0x28,0x5C,0xD0
};
static const unsigned char g_audio_capture_context[] = {
	0x43,0x06,0x2D,0xE9,0xF0,0x4F,0x06,0x46,
	0x83,0xB0,0x0C,0x46,0x40,0xF0,0xA1,0x80,
	0x3F,0x29,0x40,0xF2,0x99,0x80
};
/* r0..r3 carry ID, hardware status, user data and tag; [sp] carries bytes. */
static const unsigned char g_dmac_callback_context[] = {
	0x60,0x08,0x42,0x46,0x3B,0x46,0x00,0x95,
	0x40,0xF0,0x01,0x00,0x49,0x46,0xB0,0x47,0x05,0xB0
};
static const unsigned char g_dmac_cpu_context[] = {0x4F,0xF4,0x00,0x2E};

/* SceAudio's physical register table at text+0x7FD8. Indices 0..2 are
 * capture/loopback ports, and index 6 has no hardware register mapping. */
static const uint32_t g_i2s_register_phys[] = {
	0xE0420000u, 0xE0430000u, 0xE0440000u, 0xE0450000u,
	0xE0470000u, 0xE0460000u, 0, 0xE0490000u
};

static inline int firmware_audio_sink_matches(uint32_t destination,
	unsigned int selected_i2s, unsigned int enabled_i2s)
{
	if (selected_i2s < 3 || selected_i2s >= 8 ||
	    !g_i2s_register_phys[selected_i2s] || !(enabled_i2s & (1u << selected_i2s)))
		return 0;
	return destination == g_i2s_register_phys[selected_i2s] + 0x100u;
}

static inline int firmware_revision(enum FirmwareModule module, uint32_t nid)
{
	const uint32_t *nids;
	unsigned int count;
	if (module == FIRMWARE_UDCD) {
		nids = g_udcd_nids;
		count = sizeof(g_udcd_nids) / sizeof(g_udcd_nids[0]);
	} else if (module == FIRMWARE_AUDIO) {
		nids = g_audio_nids;
		count = sizeof(g_audio_nids) / sizeof(g_audio_nids[0]);
	} else if (module == FIRMWARE_DMAC) {
		nids = g_dmac_nids;
		count = sizeof(g_dmac_nids) / sizeof(g_dmac_nids[0]);
	} else {
		return -1;
	}
	for (unsigned int i = 0; i < count; i++)
		if (nids[i] == nid)
			return (int)i;
	/* The internal 3.650.000 tool image has different module identities,
	 * with the same executable code and private layout as external 3.65. */
	if ((module == FIRMWARE_UDCD && nid == 0xF5586BE8u) ||
	    (module == FIRMWARE_AUDIO && nid == 0x0086EFADu) ||
	    (module == FIRMWARE_DMAC && nid == 0x9480785Bu))
		return 3;
	return -1;
}

static inline int firmware_context_matches(const unsigned char *text,
	size_t text_size, size_t offset, const unsigned char *context, size_t size)
{
	if (!text || offset > text_size || size > text_size - offset)
		return 0;
	for (size_t i = 0; i < size; i++)
		if (text[offset + i] != context[i])
			return 0;
	return 1;
}

static inline int firmware_required_sizes(enum FirmwareModule module,
	uint32_t nid, size_t *text_size, size_t *data_size)
{
	int revision = firmware_revision(module, nid);
	if (revision < 0)
		return 0;
	if (module == FIRMWARE_UDCD) {
		*text_size = revision < 8 ? 0xB104u : 0xB114u;
		*data_size = 0x1760u;
	} else if (module == FIRMWARE_AUDIO) {
		*text_size = 0xD1D4u;
		*data_size = FIRMWARE_AUDIO_DATA_SIZE;
	} else {
		*text_size = 0x2CB4u;
		*data_size = 0x12A0u;
	}
	return 1;
}

static inline int firmware_module_matches(enum FirmwareModule module,
	uint32_t nid, const unsigned char *text, size_t text_size, size_t data_size)
{
	size_t required_text, required_data;
	if (!firmware_required_sizes(module, nid, &required_text, &required_data) ||
	    text_size < required_text || data_size < required_data)
		return 0;
	if (module == FIRMWARE_UDCD) {
		return firmware_context_matches(text, text_size,
			FIRMWARE_UDCD_CONFIG_OFFSET, g_udcd_config_context,
			sizeof(g_udcd_config_context));
	}
	if (module == FIRMWARE_AUDIO) {
		return firmware_context_matches(text, text_size, 0x35C4u,
			g_audio_capture_context, sizeof(g_audio_capture_context)) &&
		       firmware_context_matches(text, text_size, 0x7FD8u,
			(const unsigned char *)g_i2s_register_phys, sizeof(g_i2s_register_phys));
	}
	return firmware_context_matches(text, text_size, 0x5F0u,
		g_dmac_callback_context, sizeof(g_dmac_callback_context)) &&
	       firmware_context_matches(text, text_size, 0x1DA4u,
		g_dmac_cpu_context, sizeof(g_dmac_cpu_context));
}

#endif
