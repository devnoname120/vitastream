#ifndef VITASTREAM_FIRMWARE_H
#define VITASTREAM_FIRMWARE_H

#include <stdint.h>
#include <psp2kern/types.h>

/* Validates the loaded USB, audio and DMA modules before installing hooks. */
int firmware_prepare(SceUID *udcd_modid);

/* Revalidates SceAudio before each capture session and returns its data base. */
int firmware_audio_data(uintptr_t *data_address);

struct firmware_audio_output {
	uintptr_t fifo;
	uint32_t dma_request;
};

/* Decode routing from a base returned by firmware_audio_data for this session. */
int firmware_audio_output(uintptr_t validated_data, struct firmware_audio_output *output);

#endif
