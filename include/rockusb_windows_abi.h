#ifndef OPENROCKCLI_ROCKUSB_WINDOWS_ABI_H
#define OPENROCKCLI_ROCKUSB_WINDOWS_ABI_H
#include <stdint.h>
/* rockusb.sys 4.5 + AndroidTool 2.43; not a generic USB control IOCTL.
 * Raw payload only; CRC/padding remain in rock_maskrom_upload_memory(). */
static inline uint32_t rk_rockusb_download_ioctl(uint16_t index)
{
 switch(index) {
 case 0x0471: return UINT32_C(0x8000a000);
 case 0x0472: return UINT32_C(0x8000a004);
 default: return 0;
 }
}
#endif
