#ifndef OPENROCKCLI_USB_TRANSPORT_H
#define OPENROCKCLI_USB_TRANSPORT_H
#include <stddef.h>
#include <stdint.h>

/* Portable errors retain libusb's numeric values; OS errors are logged separately. */
enum rk_usb_error {
 RK_USB_ERROR_IO = -1, RK_USB_ERROR_INVALID_PARAM = -2,
 RK_USB_ERROR_ACCESS = -3, RK_USB_ERROR_NO_DEVICE = -4,
 RK_USB_ERROR_NOT_FOUND = -5, RK_USB_ERROR_BUSY = -6,
 RK_USB_ERROR_TIMEOUT = -7, RK_USB_ERROR_OVERFLOW = -8,
 RK_USB_ERROR_PIPE = -9, RK_USB_ERROR_INTERRUPTED = -10,
 RK_USB_ERROR_NO_MEM = -11, RK_USB_ERROR_NOT_SUPPORTED = -12
};
struct openrockcli_ctx_t;
struct rockchip_device_info;
struct rk_usb_handle;
int rk_usb_scan(struct rockchip_device_info *devices, int capacity);
int rk_usb_open(struct openrockcli_ctx_t *ctx, uint8_t bus, const char *port);
void rk_usb_close(struct openrockcli_ctx_t *ctx);
/* Inspect already-open endpoints only; never probe or claim the device. */
int rk_usb_has_bulk(const struct openrockcli_ctx_t *ctx);
int rk_usb_control(struct openrockcli_ctx_t *ctx, uint16_t code, void *data, int size);
int rk_usb_bulk(struct openrockcli_ctx_t *ctx, int input, void *data, int size, int *done);
int rk_usb_transfer_all(struct openrockcli_ctx_t *ctx, int input, void *data, size_t size);
const char *rk_usb_error_name(int error);
/* Returns an index only when the selector resolves to exactly one device. */
int rk_usb_select(const struct rockchip_device_info *devices, int count,
 const char *bus, const char *port);
#endif
