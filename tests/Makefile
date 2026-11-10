CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O1 -g
SANITIZERS ?= -fsanitize=address,undefined -fno-omit-frame-pointer
BUILD_DIR ?= .build
.DEFAULT_GOAL := test

.PHONY: test clean

$(BUILD_DIR)/video-camera: test_video_camera.c test_video.c ../src/video.c ../include/video_camera.h ../include/video.h $(BUILD_DIR)/capture.inc | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SANITIZERS) -Istubs -I../include -I$(BUILD_DIR) -DUVC_FRAMEBUFFER_COUNT=2 -DUVC_VIDEO_KERNEL_POOL -DUVC_VIDEO_CAMERA_BORROW test_video_camera.c ../src/video.c -o $@

$(BUILD_DIR)/video-camera-backend: test_video_camera_backend.c ../src/video_camera.c ../include/video_camera.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SANITIZERS) -pthread -Istubs -I../include -DUVC_VIDEO_CAMERA_BORROW test_video_camera_backend.c -o $@

test: $(BUILD_DIR)/video-double $(BUILD_DIR)/video-single $(BUILD_DIR)/video-kernel-pool $(BUILD_DIR)/video-camera $(BUILD_DIR)/video-camera-backend $(BUILD_DIR)/shutdown $(BUILD_DIR)/pacing $(BUILD_DIR)/firmware $(BUILD_DIR)/startup $(BUILD_DIR)/conversion $(BUILD_DIR)/usb-recovery $(BUILD_DIR)/usb-start $(BUILD_DIR)/suspend $(BUILD_DIR)/audio-backend $(BUILD_DIR)/config $(BUILD_DIR)/settings-oled $(BUILD_DIR)/settings-lcd $(BUILD_DIR)/audio-relay $(BUILD_DIR)/audio-route $(BUILD_DIR)/video-diagnostic
	$(BUILD_DIR)/video-double
	$(BUILD_DIR)/video-single
	$(BUILD_DIR)/video-kernel-pool
	$(BUILD_DIR)/video-camera
	$(BUILD_DIR)/video-camera-backend
	$(BUILD_DIR)/shutdown
	$(BUILD_DIR)/pacing
	$(BUILD_DIR)/firmware
	$(BUILD_DIR)/startup
	$(BUILD_DIR)/conversion
	$(BUILD_DIR)/usb-recovery
	$(BUILD_DIR)/usb-start
	$(BUILD_DIR)/suspend
	$(BUILD_DIR)/video-diagnostic
	$(BUILD_DIR)/audio-backend
	$(BUILD_DIR)/config
	$(BUILD_DIR)/settings-oled
	$(BUILD_DIR)/settings-lcd
	$(BUILD_DIR)/audio-relay
	$(BUILD_DIR)/audio-route

$(BUILD_DIR)/audio-route: test_audio_route.c ../src/audio_route.c ../include/audio_route.h stubs/taihen.h stubs/psp2kern/kernel/threadmgr/fast_mutex.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SANITIZERS) -pthread -Istubs -I../include test_audio_route.c -o $@

$(BUILD_DIR)/audio-relay: test_audio_relay.c ../src/audio_relay.c ../include/audio_relay.h ../include/firmware.h stubs/psp2kern/kernel/dmac.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SANITIZERS) -Istubs -I../include test_audio_relay.c ../src/audio_relay.c -o $@

$(BUILD_DIR)/config: test_config.c ../src/config.c ../include/config.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SANITIZERS) -I../include test_config.c ../src/config.c -o $@

$(BUILD_DIR)/settings-oled: test_settings.c ../src/config.c ../src/settings.c ../include/config.h ../include/settings.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SANITIZERS) -Istubs -I../include test_settings.c ../src/config.c ../src/settings.c -o $@

$(BUILD_DIR)/settings-lcd: test_settings.c ../src/config.c ../src/settings.c ../include/config.h ../include/settings.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SANITIZERS) -DTEST_LCD -Istubs -I../include test_settings.c ../src/config.c ../src/settings.c -o $@

$(BUILD_DIR)/usb-start.inc: ../src/main.c extract_capture.py | $(BUILD_DIR)
	python3 extract_capture.py --usb-start ../src/main.c $@

$(BUILD_DIR)/usb-start: test_usb_start.c $(BUILD_DIR)/usb-start.inc | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SANITIZERS) -I$(BUILD_DIR) test_usb_start.c -o $@

$(BUILD_DIR)/suspend.inc: ../src/main.c extract_capture.py | $(BUILD_DIR)
	python3 extract_capture.py --suspend ../src/main.c $@

$(BUILD_DIR)/suspend: test_suspend.c $(BUILD_DIR)/suspend.inc | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SANITIZERS) -I../include -I$(BUILD_DIR) test_suspend.c -o $@

$(BUILD_DIR)/video-diagnostic: test_video_diagnostic.c ../src/video_diagnostic.c ../src/diagnostic.c ../include/video_diagnostic.h ../include/diagnostic.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SANITIZERS) -DDIAGNOSTIC -pthread -Istubs -I../include test_video_diagnostic.c ../src/video_diagnostic.c ../src/diagnostic.c -o $@

$(BUILD_DIR)/audio-backend.inc: ../src/audio.c extract_capture.py | $(BUILD_DIR)
	python3 extract_capture.py --audio-backend ../src/audio.c $@

$(BUILD_DIR)/audio-backend: test_audio_backend.c ../include/uac.h ../src/audio_route.c ../include/audio_route.h stubs/psp2kern/kernel/threadmgr/fast_mutex.h $(BUILD_DIR)/audio-backend.inc | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SANITIZERS) -pthread -Istubs -I../include -I$(BUILD_DIR) test_audio_backend.c -o $@

$(BUILD_DIR)/usb-recovery.inc: ../src/main.c extract_capture.py | $(BUILD_DIR)
	python3 extract_capture.py --usb-recovery ../src/main.c $@

$(BUILD_DIR)/usb-recovery: test_usb_recovery.c $(BUILD_DIR)/usb-recovery.inc | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SANITIZERS) -I$(BUILD_DIR) test_usb_recovery.c -o $@

$(BUILD_DIR)/conversion.inc: ../src/main.c extract_capture.py | $(BUILD_DIR)
	python3 extract_capture.py --conversion ../src/main.c $@

$(BUILD_DIR)/conversion: test_conversion.c $(BUILD_DIR)/conversion.inc | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SANITIZERS) -I$(BUILD_DIR) test_conversion.c -o $@

$(BUILD_DIR)/startup.inc: ../src/main.c extract_capture.py | $(BUILD_DIR)
	python3 extract_capture.py --startup ../src/main.c $@

$(BUILD_DIR)/startup: test_startup.c ../include/firmware_profiles.h $(BUILD_DIR)/startup.inc | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SANITIZERS) -Wno-unused-parameter -Istubs -I../include -I$(BUILD_DIR) test_startup.c -o $@

$(BUILD_DIR)/firmware: test_firmware.c ../src/firmware.c ../include/firmware.h ../include/firmware_profiles.h $(wildcard stubs/*.h stubs/psp2kern/*.h stubs/psp2kern/kernel/*.h) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SANITIZERS) -Istubs -I../include test_firmware.c ../src/firmware.c -o $@

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/capture.inc: ../src/main.c extract_capture.py | $(BUILD_DIR)
	python3 extract_capture.py ../src/main.c $@

$(BUILD_DIR)/shutdown.inc: ../src/main.c extract_capture.py | $(BUILD_DIR)
	python3 extract_capture.py --shutdown ../src/main.c $@

$(BUILD_DIR)/shutdown: test_shutdown.c $(BUILD_DIR)/shutdown.inc | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SANITIZERS) -Wno-unused-parameter -Istubs -I$(BUILD_DIR) test_shutdown.c -o $@

$(BUILD_DIR)/pacing.inc: ../src/main.c ../include/usb_descriptors.h extract_capture.py | $(BUILD_DIR)
	python3 extract_capture.py --pacing ../src/main.c $@

$(BUILD_DIR)/controls.inc: ../src/main.c extract_capture.py | $(BUILD_DIR)
	python3 extract_capture.py --controls ../src/main.c $@

$(BUILD_DIR)/pacing: test_pacing.c ../include/uvc.h $(BUILD_DIR)/pacing.inc $(BUILD_DIR)/controls.inc | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SANITIZERS) -Wno-unused-parameter -I../include -I$(BUILD_DIR) test_pacing.c -o $@

$(BUILD_DIR)/video-double: test_video.c ../src/video.c ../include/video.h ../include/uvc.h $(BUILD_DIR)/capture.inc $(wildcard stubs/*.h stubs/psp2kern/*.h stubs/psp2kern/kernel/*.h) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SANITIZERS) -Istubs -I../include -I$(BUILD_DIR) -DUVC_FRAMEBUFFER_COUNT=2 test_video.c ../src/video.c -o $@

$(BUILD_DIR)/video-single: test_video.c ../src/video.c ../include/video.h ../include/uvc.h $(BUILD_DIR)/capture.inc $(wildcard stubs/*.h stubs/psp2kern/*.h stubs/psp2kern/kernel/*.h) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SANITIZERS) -Istubs -I../include -I$(BUILD_DIR) -DUVC_FRAMEBUFFER_COUNT=1 test_video.c ../src/video.c -o $@

$(BUILD_DIR)/video-kernel-pool: test_video.c ../src/video.c ../include/video.h ../include/uvc.h $(BUILD_DIR)/capture.inc $(wildcard stubs/*.h stubs/psp2kern/*.h stubs/psp2kern/kernel/*.h) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SANITIZERS) -Istubs -I../include -I$(BUILD_DIR) -DUVC_FRAMEBUFFER_COUNT=1 -DUVC_VIDEO_KERNEL_POOL test_video.c ../src/video.c -o $@

clean:
	rm -rf $(BUILD_DIR)
