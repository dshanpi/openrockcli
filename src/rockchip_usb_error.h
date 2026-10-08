#ifndef __ROCKCHIP_USB_ERROR_H__
#define __ROCKCHIP_USB_ERROR_H__

#include <usb_transport.h>

static inline const char * rockchip_usb_open_error_code(int error)
{
	switch(error)
	{
	case RK_USB_ERROR_ACCESS:
		return "ROCKCHIP_USB_ACCESS_DENIED";
	case RK_USB_ERROR_BUSY:
		return "ROCKCHIP_USB_DEVICE_BUSY";
	case RK_USB_ERROR_NOT_SUPPORTED:
	case RK_USB_ERROR_NOT_FOUND:
		return "ROCKCHIP_USB_DRIVER_UNSUPPORTED";
	case RK_USB_ERROR_NO_DEVICE:
		return "ROCKCHIP_USB_DEVICE_DISCONNECTED";
	default:
		return "ROCKCHIP_USB_OPEN_FAILED";
	}
}

static inline const char * rockchip_usb_open_recovery(int error)
{
	switch(error)
	{
	case RK_USB_ERROR_ACCESS:
		return "check USB access permissions";
	case RK_USB_ERROR_BUSY:
		return "close other Rockchip flashing tools and reconnect the device";
	case RK_USB_ERROR_NOT_SUPPORTED:
	case RK_USB_ERROR_NOT_FOUND:
		#ifdef _WIN32
        return "install the manufacturer Rockusb driver; this build uses rockusb.sys";
#else
        return "check USB driver support and reconnect the device";
#endif
	case RK_USB_ERROR_NO_DEVICE:
		return "reconnect the device and enter Maskrom again";
	default:
		return "check the USB driver, cable, and exclusive device access";
	}
}

#endif /* __ROCKCHIP_USB_ERROR_H__ */
