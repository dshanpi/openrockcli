/* Native rockusb.sys transport; see docs/WINDOWS_ROCKUSB.md for ABI evidence. */
#ifdef _WIN32
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <usbioctl.h>
#include <rockchip_backend.h>
#include <usb_transport.h>
#include <rockusb_windows_abi.h>

static const GUID usb_device_guid = {0xa5dcbf10,0x6530,0x11d2,{0x90,0x1f,0,0xc0,0x4f,0xb9,0x51,0xed}};
static const GUID usb_hub_guid = {0xf18a0e88,0xc30c,0x11d0,{0x88,0x15,0,0xa0,0xc9,6,0xbe,0xd8}};
struct rk_usb_handle { HANDLE device, input, output; };
struct windows_device {
 struct rockchip_device_info info;
 wchar_t path[1024];
};

static int windows_error(const char *operation, DWORD error)
{
 int code;
 switch(error) {
 case ERROR_ACCESS_DENIED: code = RK_USB_ERROR_ACCESS; break;
 case ERROR_SHARING_VIOLATION: case ERROR_BUSY: code = RK_USB_ERROR_BUSY; break;
 case ERROR_FILE_NOT_FOUND: case ERROR_DEVICE_NOT_CONNECTED: case ERROR_NO_SUCH_DEVICE:
  code = RK_USB_ERROR_NO_DEVICE; break;
 case ERROR_SEM_TIMEOUT: case ERROR_TIMEOUT: code = RK_USB_ERROR_TIMEOUT; break;
 case ERROR_OPERATION_ABORTED: code = RK_USB_ERROR_INTERRUPTED; break;
 case ERROR_NOT_SUPPORTED: case ERROR_INVALID_FUNCTION: code = RK_USB_ERROR_NOT_SUPPORTED; break;
 default: code = RK_USB_ERROR_IO; break;
 }
 fprintf(stderr, "ROCKCHIP_WINDOWS_ERROR: operation=%s win32=%lu (%s)\n",
  operation, (unsigned long)error, rk_usb_error_name(code));
 return code;
}

/* Read descriptors from the parent hub, without opening/claiming the target. */
static int device_descriptor(HDEVINFO set, SP_DEVINFO_DATA *dev, USB_DEVICE_DESCRIPTOR *desc, int *maskrom)
{
 DEVINST parent;
 wchar_t instance[MAX_DEVICE_ID_LEN], *paths = NULL;
 ULONG chars = 0;
 DWORD index = 0, type = 0, got = 0;
 HANDLE hub = INVALID_HANDLE_VALUE;
 struct {
  USB_NODE_CONNECTION_INFORMATION_EX connection;
  USB_PIPE_INFO pipes[32];
 } info;
 int result = RK_USB_ERROR_IO;
 if(!SetupDiGetDeviceRegistryPropertyW(set, dev, SPDRP_ADDRESS, &type,
    (BYTE *)&index, sizeof(index), NULL) || type != REG_DWORD || !index)
  return RK_USB_ERROR_NOT_SUPPORTED;
 if(CM_Get_Parent(&parent, dev->DevInst, 0) != CR_SUCCESS ||
    CM_Get_Device_IDW(parent, instance, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS ||
    CM_Get_Device_Interface_List_SizeW(&chars, (GUID *)&usb_hub_guid, instance,
     CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || chars < 2)
  return RK_USB_ERROR_NOT_SUPPORTED;
 paths = calloc(chars, sizeof(*paths));
 if(!paths) return RK_USB_ERROR_NO_MEM;
 if(CM_Get_Device_Interface_ListW((GUID *)&usb_hub_guid, instance, paths, chars,
    CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || !paths[0]) goto cleanup;
 hub = CreateFileW(paths, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
  NULL, OPEN_EXISTING, 0, NULL);
 if(hub == INVALID_HANDLE_VALUE) { result = windows_error("open parent hub", GetLastError()); goto cleanup; }
 memset(&info, 0, sizeof(info));
 info.connection.ConnectionIndex = index;
 if(!DeviceIoControl(hub, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX,
    &info, sizeof(info), &info, sizeof(info), &got, NULL)) {
  result = windows_error("query USB descriptor", GetLastError()); goto cleanup;
 }
 if(got < offsetof(USB_NODE_CONNECTION_INFORMATION_EX, PipeList) ||
    info.connection.ConnectionStatus != DeviceConnected ||
    info.connection.DeviceDescriptor.bLength != sizeof(*desc)) goto cleanup;
 *desc = info.connection.DeviceDescriptor;
 *maskrom = !(desc->bcdUSB & 1);
 if(*maskrom && desc->iProduct) {
  unsigned char buffer[sizeof(USB_DESCRIPTOR_REQUEST) + 256] = {0};
  USB_DESCRIPTOR_REQUEST *request = (USB_DESCRIPTOR_REQUEST *)buffer;
  request->ConnectionIndex = index;
  request->SetupPacket.bmRequest = 0x80;
  request->SetupPacket.bRequest = 6;
  request->SetupPacket.wValue = (3 << 8) | desc->iProduct;
  request->SetupPacket.wIndex = 0x0409;
  request->SetupPacket.wLength = 255;
  if(DeviceIoControl(hub, IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION,
     buffer, sizeof(buffer), buffer, sizeof(buffer), &got, NULL) &&
     got >= sizeof(USB_DESCRIPTOR_REQUEST) + 2) {
   unsigned char *raw = request->Data;
   unsigned n = raw[0];
   if(raw[1] == 3 && n >= 2 && !(n & 1) && n <= 255 &&
      got >= offsetof(USB_DESCRIPTOR_REQUEST, Data) + n) {
    wchar_t product[128] = {0};
    for(unsigned i = 2; i < n; i += 2) product[(i - 2) / 2] = raw[i] | (raw[i + 1] << 8);
    for(unsigned i = 0; i < (n - 2) / 2; i++)
     if(product[i] >= L'A' && product[i] <= L'Z') product[i] += L'a' - L'A';
    if(wcsstr(product, L"usb-msc") || wcsstr(product, L"loader") || wcsstr(product, L"download"))
     *maskrom = 0;
   }
  }
 }
 result = 0;
cleanup:
 if(hub != INVALID_HANDLE_VALUE) CloseHandle(hub);
 free(paths);
 return result;
}

static int enumerate(struct windows_device *out, int capacity)
{
 HDEVINFO set = SetupDiGetClassDevsW(&usb_device_guid, NULL, NULL,
  DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
 int count = 0, error = 0;
 if(set == INVALID_HANDLE_VALUE) return windows_error("enumerate", GetLastError());
 for(DWORD i = 0; ; i++) {
  SP_DEVICE_INTERFACE_DATA iface = { .cbSize = sizeof(iface) };
  SP_DEVINFO_DATA dev = { .cbSize = sizeof(dev) };
  SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail;
  wchar_t ids[2048] = {0}, service[128] = {0}, location[1024] = {0};
  USB_DEVICE_DESCRIPTOR desc = {0};
  DWORD size = 0, type = 0;
  int maskrom = 1;
  char *utf8;
  if(!SetupDiEnumDeviceInterfaces(set, NULL, &usb_device_guid, i, &iface)) {
   DWORD last = GetLastError();
   if(last != ERROR_NO_MORE_ITEMS) error = windows_error("enumerate interface", last);
   break;
  }
  SetupDiGetDeviceInterfaceDetailW(set, &iface, NULL, 0, &size, NULL);
  if(!size) { error = windows_error("size interface", GetLastError()); break; }
  detail = calloc(1, size);
  if(!detail) { error = RK_USB_ERROR_NO_MEM; break; }
  detail->cbSize = sizeof(*detail);
  if(!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, size, NULL, &dev)) {
   error = windows_error("read interface", GetLastError()); free(detail); break;
  }
  if(!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, &type,
     (BYTE *)ids, sizeof(ids) - sizeof(wchar_t), NULL)) { free(detail); continue; }
  /* Hardware ID is OS-generated; only exact Rockchip VID and a non-composite interface. */
  if(_wcsnicmp(ids, L"USB\\VID_2207&PID_", 17)) { free(detail); continue; }
  if(wcsstr(ids, L"&MI_")) { free(detail); continue; }
  if(!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_SERVICE, &type,
     (BYTE *)service, sizeof(service) - sizeof(wchar_t), NULL) || _wcsicmp(service, L"rockusb")) {
   fprintf(stderr, "ROCKCHIP_USB_DRIVER_UNSUPPORTED: native Windows build requires rockusb.sys\n");
   error = RK_USB_ERROR_NOT_SUPPORTED; free(detail); break;
  }
  if(count >= capacity || wcslen(detail->DevicePath) >= 1024) {
   error = RK_USB_ERROR_OVERFLOW; free(detail); break;
  }
  if(!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_LOCATION_PATHS, &type,
     (BYTE *)location, sizeof(location) - sizeof(wchar_t), NULL) || !location[0]) {
   error = RK_USB_ERROR_NOT_SUPPORTED; free(detail); break;
  }
  utf8 = rk_utf8_from_wide(location);
  if(!utf8 || strlen(utf8) >= sizeof(out[count].info.port)) {
   error = RK_USB_ERROR_OVERFLOW; free(utf8); free(detail); break;
  }
  error = device_descriptor(set, &dev, &desc, &maskrom);
  if(error) { free(utf8); free(detail); break; }
  if(desc.idVendor != 0x2207) {
   error = RK_USB_ERROR_NO_DEVICE; free(utf8); free(detail); break;
  }
  memset(&out[count], 0, sizeof(out[count]));
  wcscpy(out[count].path, detail->DevicePath);
  strcpy(out[count].info.port, utf8);
  out[count].info.vid = desc.idVendor;
  out[count].info.pid = desc.idProduct;
  /* Rockchip's descriptor convention, identical to the existing libusb backend. */
  strcpy(out[count].info.mode, maskrom ? "maskrom" : "loader");
  snprintf(out[count].info.chip, sizeof(out[count].info.chip), "%s", openrockcli_chip_name(desc.idProduct));
  count++;
  free(utf8); free(detail);
 }
 SetupDiDestroyDeviceInfoList(set);
 return error ? error : count;
}

int rk_usb_scan(struct rockchip_device_info *devices, int capacity)
{
 struct windows_device *list;
 int count;
 if(!devices || capacity <= 0) return RK_USB_ERROR_INVALID_PARAM;
 list = calloc(capacity, sizeof(*list));
 if(!list) return RK_USB_ERROR_NO_MEM;
 count = enumerate(list, capacity);
 for(int i = 0; i < count; i++) devices[i] = list[i].info;
 free(list);
 return count;
}

/* rockusb.sys exposes bulk endpoints as child files, not the control handle.
 * AndroidTool 2.43's open routine (0x4834c0): PIPE00 read, PIPE01 write.
 * Maskrom only needs the base handle; pipes are opened after re-enumeration. */
static int open_windows_device(struct openrockcli_ctx_t *ctx, const struct windows_device *dev)
{
 wchar_t path[1024 + 8];
 int result;
 if(wcslen(dev->path) >= 1024) return RK_USB_ERROR_OVERFLOW;
 ctx->usb = calloc(1, sizeof(*ctx->usb));
 if(!ctx->usb) return RK_USB_ERROR_NO_MEM;
 ctx->usb->device = ctx->usb->input = ctx->usb->output = INVALID_HANDLE_VALUE;
 ctx->pid = dev->info.pid;
 ctx->maskrom = !strcmp(dev->info.mode, "maskrom");
 ctx->usb->device = CreateFileW(dev->path, GENERIC_READ | GENERIC_WRITE,
  FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
 if(ctx->usb->device == INVALID_HANDLE_VALUE) {
  result = windows_error("open Rockusb control", GetLastError()); goto failed;
 }
 if(!ctx->maskrom) {
  wcscpy(path, dev->path); wcscat(path, L"\\PIPE00");
  ctx->usb->input = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ,
   NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
  if(ctx->usb->input == INVALID_HANDLE_VALUE) {
   result = windows_error("open Rockusb PIPE00 (read)", GetLastError()); goto failed;
  }
  wcscpy(path, dev->path); wcscat(path, L"\\PIPE01");
  ctx->usb->output = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_WRITE,
   NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
  if(ctx->usb->output == INVALID_HANDLE_VALUE) {
   result = windows_error("open Rockusb PIPE01 (write)", GetLastError()); goto failed;
  }
 }
 return 0;
failed:
 rk_usb_close(ctx);
 return result;
}

int rk_usb_open(struct openrockcli_ctx_t *ctx, uint8_t bus, const char *port)
{
 struct windows_device *list = calloc(64, sizeof(*list));
 int count, selected = -1, result = RK_USB_ERROR_NO_DEVICE;
 if(!list) return RK_USB_ERROR_NO_MEM;
 count = enumerate(list, 64);
 if(count < 0) { result = count; goto cleanup; }
 for(int i = 0; i < count; i++) {
  if(bus != 0 || (port && strcmp(list[i].info.port, port))) continue;
  if(selected >= 0) { result = RK_USB_ERROR_BUSY; goto cleanup; }
  selected = i;
 }
 if(selected < 0) goto cleanup;
 result = open_windows_device(ctx, &list[selected]);
cleanup:
 free(list);
 return result;
}
void rk_usb_close(struct openrockcli_ctx_t *ctx)
{
 if(!ctx || !ctx->usb) return;
 if(ctx->usb->output != INVALID_HANDLE_VALUE) CloseHandle(ctx->usb->output);
 if(ctx->usb->input != INVALID_HANDLE_VALUE) CloseHandle(ctx->usb->input);
 if(ctx->usb->device != INVALID_HANDLE_VALUE) CloseHandle(ctx->usb->device);
 free(ctx->usb);
 ctx->usb = NULL;
}

/* Keep OVERLAPPED and buffers alive until cancellation has completed. No retry. */
static int complete(struct openrockcli_ctx_t *ctx, HANDLE handle, OVERLAPPED *ov, BOOL immediate,
 DWORD initial_error, DWORD *done)
{
 int result = 0;
 if(!immediate && initial_error != ERROR_IO_PENDING)
  result = windows_error("submit transfer", initial_error);
 else {
  DWORD wait = immediate ? WAIT_OBJECT_0 : WaitForSingleObject(ov->hEvent, 10000);
  if(wait != WAIT_OBJECT_0) {
   DWORD wait_error = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
   CancelIoEx(handle, ov);
   GetOverlappedResult(handle, ov, done, TRUE);
   result = windows_error("wait transfer", wait_error);
  } else if(!GetOverlappedResult(handle, ov, done, FALSE))
   result = windows_error("complete transfer", GetLastError());
 }
 CloseHandle(ov->hEvent);
 if(result) ctx->last_usb_error = result;
 return result;
}
int rk_usb_has_bulk(const struct openrockcli_ctx_t *ctx)
{
 return ctx && ctx->usb && ctx->usb->input != INVALID_HANDLE_VALUE &&
  ctx->usb->output != INVALID_HANDLE_VALUE;
}
int rk_usb_control(struct openrockcli_ctx_t *ctx, uint16_t code, void *data, int size)
{
 OVERLAPPED ov = {0};
 DWORD done = 0, ioctl = rk_rockusb_download_ioctl(code);
 BOOL ok;
 int rc;
 if(!ctx || !ctx->usb || !ioctl || size < 1 || size > 4096) return RK_USB_ERROR_INVALID_PARAM;
 ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
 if(!ov.hEvent) return windows_error("create event", GetLastError());
 ok = DeviceIoControl(ctx->usb->device, ioctl, data, size, NULL, 0, &done, &ov);
 rc = complete(ctx, ctx->usb->device, &ov, ok, ok ? 0 : GetLastError(), &done);
 return rc ? rc : (int)done;
}
int rk_usb_bulk(struct openrockcli_ctx_t *ctx, int input, void *data, int size, int *done)
{
 OVERLAPPED ov = {0};
 DWORD bytes = 0;
 BOOL ok;
 HANDLE handle;
 int rc;
 if(!done) return RK_USB_ERROR_INVALID_PARAM;
 *done = 0;
 if(!ctx || !ctx->usb || !data || size <= 0) return RK_USB_ERROR_INVALID_PARAM;
 handle = input ? ctx->usb->input : ctx->usb->output;
 if(handle == INVALID_HANDLE_VALUE) return RK_USB_ERROR_NOT_SUPPORTED;
 ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
 if(!ov.hEvent) return windows_error("create event", GetLastError());
 ok = input ? ReadFile(handle, data, size, &bytes, &ov) :
  WriteFile(handle, data, size, &bytes, &ov);
 rc = complete(ctx, handle, &ov, ok, ok ? 0 : GetLastError(), &bytes);
 if(!rc) *done = (int)bytes;
 return rc;
}
#endif
