#include <assert.h>
#include <string.h>
#include <rockchip_usb_error.h>

int main(void)
{
	assert(!strcmp(rockchip_usb_open_error_code(RK_USB_ERROR_ACCESS),
		"ROCKCHIP_USB_ACCESS_DENIED"));
	assert(!strcmp(rockchip_usb_open_error_code(RK_USB_ERROR_BUSY),
		"ROCKCHIP_USB_DEVICE_BUSY"));
	assert(!strcmp(rockchip_usb_open_error_code(RK_USB_ERROR_NOT_SUPPORTED),
		"ROCKCHIP_USB_DRIVER_UNSUPPORTED"));
	assert(!strcmp(rockchip_usb_open_error_code(RK_USB_ERROR_NOT_FOUND),
		"ROCKCHIP_USB_DRIVER_UNSUPPORTED"));
	assert(!strcmp(rockchip_usb_open_error_code(RK_USB_ERROR_NO_DEVICE),
		"ROCKCHIP_USB_DEVICE_DISCONNECTED"));
	assert(!strcmp(rockchip_usb_open_error_code(RK_USB_ERROR_IO),
		"ROCKCHIP_USB_OPEN_FAILED"));
	assert(strstr(rockchip_usb_open_recovery(RK_USB_ERROR_BUSY), "close other"));
	assert(strstr(rockchip_usb_open_recovery(RK_USB_ERROR_NOT_SUPPORTED), "driver"));
	return 0;
}
