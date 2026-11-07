#ifndef VITASTREAM_USB_STATE_H
#define VITASTREAM_USB_STATE_H

/* Native SceUdcd ABI, verified in the firmware. The SDK's driver-state
 * labels are reversed and its ACTIVATED constant is not the native bit. */
enum {
	VITASTREAM_USB_DRIVER_ABSENT = -1,
	VITASTREAM_USB_DRIVER_REGISTERED = 1,
	VITASTREAM_USB_DRIVER_STARTED = 2,
	VITASTREAM_USB_ACTIVATED = 0x2000,
	VITASTREAM_USB_CONNECTED = 0x20
};

#endif
