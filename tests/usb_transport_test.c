#include <assert.h>
#include <string.h>
#include <rockchip_backend.h>
#include <usb_transport.h>
#include <rockusb_windows_abi.h>
static int scenario, calls;
int rk_usb_bulk(struct openrockcli_ctx_t *ctx, int input, void *data, int size, int *done)
{
 (void)ctx; (void)input; (void)data;
 calls++;
 if(scenario == 1) { *done = 0; return 0; }
 if(scenario == 2) { *done = size + 1; return 0; }
 if(scenario == 3) return RK_USB_ERROR_TIMEOUT;
 *done = size > 3 ? 3 : size;
 return 0;
}
int main(void)
{
 assert(rk_rockusb_download_ioctl(0x471) == 0x8000a000U);
 assert(rk_rockusb_download_ioctl(0x472) == 0x8000a004U);
 assert(rk_rockusb_download_ioctl(0x473) == 0);
 struct openrockcli_ctx_t ctx = {0};
 char data[10] = {0};
 struct rockchip_device_info devices[2] = {
  {.bus=1, .port="2.3"}, {.bus=2, .port="2.3"}
 };
 assert(rk_usb_select(devices, 2, NULL, NULL) == RK_USB_ERROR_BUSY);
 assert(rk_usb_select(devices, 2, NULL, "2.3") == RK_USB_ERROR_BUSY);
 assert(rk_usb_select(devices, 2, "2", "2.3") == 1);
 assert(rk_usb_select(devices, 1, "2", "2.3") == RK_USB_ERROR_NO_DEVICE);
 assert(rk_usb_select(devices, 2, "258", NULL) == RK_USB_ERROR_INVALID_PARAM);
 assert(rk_usb_select(devices, 2, "2oops", NULL) == RK_USB_ERROR_INVALID_PARAM);
 assert(rk_usb_select(devices, 2, "", NULL) == RK_USB_ERROR_INVALID_PARAM);
 assert(rk_usb_select(devices, 0, NULL, NULL) == RK_USB_ERROR_NO_DEVICE);
 assert(rk_usb_transfer_all(&ctx, 0, data, sizeof(data)) == 1 && calls == 4);
 for(scenario = 1; scenario <= 3; scenario++) {
  calls = 0;
  assert(!rk_usb_transfer_all(&ctx, 1, data, sizeof(data)));
  assert(calls == 1); /* No infinite loop and no automatic retry. */
  assert(ctx.last_usb_error < 0);
 }
 return 0;
}
