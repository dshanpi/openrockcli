#include <rockchip_backend.h>
#include <usb_transport.h>

int rk_usb_transfer_all(struct openrockcli_ctx_t *ctx, int input, void *data, size_t size)
{
 unsigned char *p = data;
 while(size) {
  int done = 0;
  int chunk = size > 65536 ? 65536 : (int)size;
  int rc = rk_usb_bulk(ctx, input, p, chunk, &done);
  if(rc || done <= 0 || done > chunk) {
   ctx->last_usb_error = rc ? rc : RK_USB_ERROR_IO;
   fprintf(stderr, "ROCKCHIP_USB_TRANSFER_FAILED: %s, direction=%s, requested=%d, transferred=%d\n",
    rk_usb_error_name(ctx->last_usb_error), input ? "read" : "write", chunk, done);
   return 0;
  }
  size -= done;
  p += done;
 }
 return 1;
}

int rk_usb_select(const struct rockchip_device_info *devices, int count,
 const char *bus, const char *port)
{
 unsigned long number = 0;
 int selected = -1;
 if(bus) {
  char *end;
  if(!*bus || !isdigit((unsigned char)*bus)) return RK_USB_ERROR_INVALID_PARAM;
  errno = 0;
  number = strtoul(bus, &end, 10);
  if(errno || *end || number > 255) return RK_USB_ERROR_INVALID_PARAM;
 }
 if(port && !*port) return RK_USB_ERROR_INVALID_PARAM;
 for(int i = 0; i < count; i++) {
  if(bus && devices[i].bus != number) continue;
  if(port && strcmp(devices[i].port, port)) continue;
  if(!devices[i].port[0]) return RK_USB_ERROR_NOT_SUPPORTED;
  if(selected >= 0) return RK_USB_ERROR_BUSY;
  selected = i;
 }
 return selected < 0 ? RK_USB_ERROR_NO_DEVICE : selected;
}

const char *rk_usb_error_name(int error)
{
 switch(error) {
 case 0: return "success";
 case RK_USB_ERROR_IO: return "I/O error";
 case RK_USB_ERROR_INVALID_PARAM: return "invalid parameter";
 case RK_USB_ERROR_ACCESS: return "access denied";
 case RK_USB_ERROR_NO_DEVICE: return "device disconnected or not found";
 case RK_USB_ERROR_NOT_FOUND: return "interface not found";
 case RK_USB_ERROR_BUSY: return "device busy or ambiguous selection";
 case RK_USB_ERROR_TIMEOUT: return "timeout";
 case RK_USB_ERROR_OVERFLOW: return "buffer overflow";
 case RK_USB_ERROR_PIPE: return "pipe stalled";
 case RK_USB_ERROR_INTERRUPTED: return "interrupted";
 case RK_USB_ERROR_NO_MEM: return "out of memory";
 case RK_USB_ERROR_NOT_SUPPORTED: return "unsupported driver or operation";
 default: return "unknown USB error";
 }
}
