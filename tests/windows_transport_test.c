/* Compile with MinGW. Exercise the real native adapter using mocked Win32 I/O. */
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <usbioctl.h>
#include <assert.h>
#include <rockchip_backend.h>

static DWORD last_error, mode, observed_ioctl, length;
static unsigned reads, writes, cancels, closes, queries, submits;
static unsigned opens, handle_closes, live_handles, fail_open;
static HANDLE pending_handle;
static const wchar_t target_path[] = L"\\\\?\\usb#vid_2207&pid_350b#测试";
static int enum_count = 1, enum_driver_bad, enum_missing_location, enum_disconnect;
static const wchar_t location[] = L"PCIROOT(0)#USBROOT(0)#USB(1)";
static HDEVINFO WINAPI fake_classes(const GUID *g, PCWSTR e, HWND w, DWORD flags)
{ (void)g;(void)e;(void)w; assert(flags == (DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)); return (HDEVINFO)6; }
static BOOL WINAPI fake_interfaces(HDEVINFO h, PSP_DEVINFO_DATA d, const GUID *g, DWORD n, PSP_DEVICE_INTERFACE_DATA out)
{ (void)d;(void)g;assert(h==(HDEVINFO)6);if(n >= (DWORD)enum_count) {last_error=ERROR_NO_MORE_ITEMS;return FALSE;}out->Reserved=n;return TRUE; }
static BOOL WINAPI fake_detail(HDEVINFO h, PSP_DEVICE_INTERFACE_DATA i, PSP_DEVICE_INTERFACE_DETAIL_DATA_W out,
 DWORD size, PDWORD needed, PSP_DEVINFO_DATA d)
{
 assert(h==(HDEVINFO)6); (void)i;
 DWORD n=sizeof(*out)+(DWORD)sizeof(target_path);
 if(needed) *needed=n;
 if(!out) {last_error=ERROR_INSUFFICIENT_BUFFER;return FALSE;}
 assert(size>=n && out->cbSize==sizeof(*out));wcscpy(out->DevicePath,target_path);d->DevInst=1;return TRUE;
}
static BOOL WINAPI fake_property(HDEVINFO h, PSP_DEVINFO_DATA d, DWORD prop, PDWORD type, PBYTE out, DWORD size, PDWORD needed)
{
 (void)d;(void)needed;assert(h==(HDEVINFO)6);
 const wchar_t *value=NULL;
 if(prop==SPDRP_ADDRESS) {assert(size>=4);*(DWORD*)out=1;*type=REG_DWORD;return TRUE;}
 if(prop==SPDRP_HARDWAREID) value=L"USB\\VID_2207&PID_350B";
 if(prop==SPDRP_SERVICE) value=enum_driver_bad?L"WinUSB":L"rockusb";
 if(prop==SPDRP_LOCATION_PATHS) {if(enum_missing_location)return FALSE;value=location;}
 assert(value && size>=(wcslen(value)+2)*sizeof(wchar_t));*type=prop==SPDRP_SERVICE?REG_SZ:REG_MULTI_SZ;
 memset(out,0,size);wcscpy((wchar_t*)out,value);return TRUE;
}
static BOOL WINAPI fake_destroy(HDEVINFO h) {assert(h==(HDEVINFO)6);return TRUE;}
static CONFIGRET WINAPI fake_parent(PDEVINST p, DEVINST d, ULONG flags) {(void)d;(void)flags;*p=2;return CR_SUCCESS;}
static CONFIGRET WINAPI fake_id(DEVINST d, PWSTR p, ULONG size, ULONG flags) {(void)d;(void)flags;assert(size>4);wcscpy(p,L"hub");return CR_SUCCESS;}
static CONFIGRET WINAPI fake_list_size(PULONG size, LPGUID g, DEVINSTID_W d, ULONG flags) {(void)g;(void)d;(void)flags;*size=5;return CR_SUCCESS;}
static CONFIGRET WINAPI fake_list(LPGUID g, DEVINSTID_W d, PZZWSTR p, ULONG size, ULONG flags) {(void)g;(void)d;(void)flags;assert(size>=5);memset(p,0,size*sizeof(wchar_t));wcscpy(p,L"hub");return CR_SUCCESS;}
#define SetupDiGetClassDevsW fake_classes
#define SetupDiEnumDeviceInterfaces fake_interfaces
#define SetupDiGetDeviceInterfaceDetailW fake_detail
#define SetupDiGetDeviceRegistryPropertyW fake_property
#define SetupDiDestroyDeviceInfoList fake_destroy
#define CM_Get_Parent fake_parent
#define CM_Get_Device_IDW fake_id
#define CM_Get_Device_Interface_List_SizeW fake_list_size
#define CM_Get_Device_Interface_ListW fake_list
static HANDLE WINAPI fake_open(LPCWSTR path, DWORD access, DWORD share,
 LPSECURITY_ATTRIBUTES security, DWORD creation, DWORD flags, HANDLE template)
{
 if(!wcscmp(path,L"hub")) { assert(flags==0 && access==GENERIC_WRITE); return (HANDLE)5; }
 wchar_t expected[1024];
 HANDLE handle;
 unsigned bit;
 assert(!security && !template && creation == OPEN_EXISTING && flags == FILE_FLAG_OVERLAPPED);
 wcscpy(expected, target_path);
 if(access == GENERIC_READ) {
  wcscat(expected, L"\\PIPE00"); handle = (HANDLE)3; bit = 2; assert(share == FILE_SHARE_READ);
 } else if(access == GENERIC_WRITE) {
  wcscat(expected, L"\\PIPE01"); handle = (HANDLE)4; bit = 4; assert(share == FILE_SHARE_WRITE);
 } else {
  assert(access == (GENERIC_READ | GENERIC_WRITE));
  assert(share == (FILE_SHARE_READ | FILE_SHARE_WRITE)); handle = (HANDLE)1; bit = 1;
 }
 assert(!wcscmp(path, expected));
 opens++;
 if(opens == fail_open) { last_error = ERROR_ACCESS_DENIED; return INVALID_HANDLE_VALUE; }
 assert(!(live_handles & bit)); live_handles |= bit;
 return handle;
}
static HANDLE WINAPI fake_event(LPSECURITY_ATTRIBUTES a, BOOL b, BOOL c, LPCWSTR d)
{ (void)a; (void)b; (void)c; (void)d; return (HANDLE)2; }
static DWORD WINAPI fake_error(void) { return last_error; }
static BOOL WINAPI fake_close(HANDLE h)
{
 if(h == (HANDLE)5) return TRUE;
 if(h == (HANDLE)2) closes++;
 else {
  unsigned bit = h == (HANDLE)1 ? 1 : h == (HANDLE)3 ? 2 : h == (HANDLE)4 ? 4 : 0;
  assert(bit && (live_handles & bit)); live_handles &= ~bit; handle_closes++;
 }
 return TRUE;
}
static DWORD WINAPI fake_wait(HANDLE h, DWORD ms)
{ assert(h == (HANDLE)2 && ms == 10000); return mode == 2 ? WAIT_TIMEOUT : WAIT_OBJECT_0; }
static BOOL WINAPI fake_cancel(HANDLE h, LPOVERLAPPED ov)
{ assert(h == pending_handle && ov); cancels++; return TRUE; }
static BOOL WINAPI fake_result(HANDLE h, LPOVERLAPPED ov, LPDWORD n, BOOL wait)
{
 assert(h == pending_handle && ov);
 queries++;
 if(mode == 2) assert(wait); else assert(!wait);
 *n = length;
 return TRUE;
}
static BOOL submitted(HANDLE h, DWORD n)
{
 submits++; length = n; pending_handle = h;
 last_error = mode == 3 ? ERROR_ACCESS_DENIED : ERROR_IO_PENDING;
 return mode == 0;
}
static BOOL WINAPI fake_ioctl(HANDLE h, DWORD code, LPVOID in, DWORD inlen,
 LPVOID out, DWORD outlen, LPDWORD done, LPOVERLAPPED ov)
{
 if(h==(HANDLE)5) {
  assert(code==IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX && !ov && in==out);
  USB_NODE_CONNECTION_INFORMATION_EX *info=out;
  info->ConnectionStatus=enum_disconnect?NoDeviceConnected:DeviceConnected;
  info->DeviceDescriptor.bLength=sizeof(USB_DEVICE_DESCRIPTOR);info->DeviceDescriptor.idVendor=0x2207;
  info->DeviceDescriptor.idProduct=0x350b;info->DeviceDescriptor.bcdUSB=0x201;*done=outlen;return TRUE;
 }
 assert(h == (HANDLE)1 && in && ov && done);
 assert(!out && !outlen && inlen <= 4096);
 observed_ioctl = code;
 return submitted(h, inlen);
}
static BOOL WINAPI fake_read(HANDLE h, LPVOID data, DWORD n, LPDWORD done, LPOVERLAPPED ov)
{ assert(data && done && ov); reads++; if(h != (HANDLE)3) { last_error = ERROR_INVALID_HANDLE; return FALSE; } return submitted(h, n); }
static BOOL WINAPI fake_write(HANDLE h, LPCVOID data, DWORD n, LPDWORD done, LPOVERLAPPED ov)
{ assert(data && done && ov); writes++; if(h != (HANDLE)4) { last_error = ERROR_INVALID_HANDLE; return FALSE; } return submitted(h, n); }
#define CreateFileW fake_open
#define CreateEventW fake_event
#define GetLastError fake_error
#define CloseHandle fake_close
#define WaitForSingleObject fake_wait
#define CancelIoEx fake_cancel
#define GetOverlappedResult fake_result
#define DeviceIoControl fake_ioctl
#define ReadFile fake_read
#define WriteFile fake_write
#include "../src/usb_windows.c"
const char *openrockcli_chip_name(uint16_t pid) { (void)pid; return "mock"; }
int main(void)
{
 struct openrockcli_ctx_t ctx = {0};
 struct windows_device dev = {0};
 wcscpy(dev.path, target_path); strcpy(dev.info.mode, "loader"); dev.info.pid = 0x350b;
 assert(open_windows_device(&ctx, &dev) == 0);
 assert(opens == 3 && live_handles == 7 && ctx.pid == 0x350b && !ctx.maskrom);
 assert(rk_usb_has_bulk(&ctx));
 char data[4096] = {0};
 int done = 0;
 assert(rk_usb_control(&ctx, 0x471, data, 4096) == 4096);
 assert(observed_ioctl == 0x8000a000 && queries == 1 && closes == 1);
 mode = 1;
 assert(rk_usb_control(&ctx, 0x472, data, 7) == 7);
 assert(observed_ioctl == 0x8000a004 && !cancels);
 assert(rk_usb_bulk(&ctx, 1, data, 13, &done) == 0 && done == 13 && reads == 1);
 assert(rk_usb_bulk(&ctx, 0, data, 31, &done) == 0 && done == 31 && writes == 1);
 mode = 2;
 assert(rk_usb_bulk(&ctx, 0, data, 31, &done) == RK_USB_ERROR_TIMEOUT);
 assert(cancels == 1 && done == 0 && ctx.last_usb_error == RK_USB_ERROR_TIMEOUT);
 mode = 3;
 unsigned before = queries;
 assert(rk_usb_control(&ctx, 0x471, data, 10) == RK_USB_ERROR_ACCESS);
 assert(queries == before && cancels == 1);
 before = submits;
 assert(rk_usb_control(&ctx, 0x473, data, 10) == RK_USB_ERROR_INVALID_PARAM);
 assert(rk_usb_control(&ctx, 0x471, data, 4097) == RK_USB_ERROR_INVALID_PARAM);
 assert(submits == before && submits == closes);
 rk_usb_close(&ctx);
 assert(!rk_usb_has_bulk(&ctx) && !rk_usb_has_bulk(NULL));
 assert(!ctx.usb && !live_handles && handle_closes == 3);
 rk_usb_close(&ctx); /* Idempotent; no second close. */
 assert(handle_closes == 3);
 for(unsigned fail = 1; fail <= 3; fail++) {
  opens = handle_closes = 0; fail_open = fail;
  assert(open_windows_device(&ctx, &dev) == RK_USB_ERROR_ACCESS);
  assert(!ctx.usb && !live_handles && opens == fail && handle_closes == fail - 1);
 }
 fail_open = opens = handle_closes = 0;
 strcpy(dev.info.mode, "maskrom");
 assert(open_windows_device(&ctx, &dev) == 0 && ctx.maskrom && opens == 1 && live_handles == 1);
 assert(!rk_usb_has_bulk(&ctx));
 before = submits;
 assert(rk_usb_bulk(&ctx, 0, data, 31, &done) == RK_USB_ERROR_NOT_SUPPORTED && !done);
 assert(submits == before); /* Never submit bulk to the control handle. */
 mode = 0;
 assert(rk_usb_control(&ctx, 0x471, data, 7) == 7);
 rk_usb_close(&ctx);
 assert(!live_handles && handle_closes == 1);
 strcpy(dev.info.mode, "loader"); opens = 0;
 assert(open_windows_device(&ctx, &dev) == 0 && opens == 3); /* After re-enumeration. */
 mode = 0;
 assert(rk_usb_bulk(&ctx, 0, data, 31, &done) == 0 && done == 31);
 mode = 2;
 assert(rk_usb_bulk(&ctx, 1, data, 13, &done) == RK_USB_ERROR_TIMEOUT && !done);
 rk_usb_close(&ctx);
 assert(!live_handles);
 /* Exercise real SetupAPI selection and parent-hub descriptor code. No USB I/O. */
 struct rockchip_device_info scanned[2];
 assert(rk_usb_scan(scanned,2)==1 && !strcmp(scanned[0].mode,"loader") && !strcmp(scanned[0].port,"PCIROOT(0)#USBROOT(0)#USB(1)"));
 unsigned before_opens=opens;
 enum_count=2;
 assert(rk_usb_open(&ctx,0,scanned[0].port)==RK_USB_ERROR_BUSY && opens==before_opens);
 enum_count=1;
 assert(rk_usb_open(&ctx,0,"PCIROOT(0)#USBROOT(0)#USB(2)")==RK_USB_ERROR_NO_DEVICE && opens==before_opens);
 assert(rk_usb_open(&ctx,1,scanned[0].port)==RK_USB_ERROR_NO_DEVICE && opens==before_opens);
 assert(rk_usb_open(&ctx,0,scanned[0].port)==0 && opens==before_opens+3);
 rk_usb_close(&ctx); assert(!live_handles);
 enum_driver_bad=1;assert(rk_usb_scan(scanned,2)==RK_USB_ERROR_NOT_SUPPORTED);enum_driver_bad=0;
 enum_missing_location=1;assert(rk_usb_scan(scanned,2)==RK_USB_ERROR_NOT_SUPPORTED);enum_missing_location=0;
 enum_disconnect=1;assert(rk_usb_scan(scanned,2)<0);enum_disconnect=0;
 enum_count=0;assert(rk_usb_scan(scanned,2)==0);
 return 0;
}
