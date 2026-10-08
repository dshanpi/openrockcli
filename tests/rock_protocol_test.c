/* End-to-end firmware workflow over an in-memory USB transport; never opens hardware. */
#include <rockchip_backend.h>
#include <rkimage.h>
#include <usb_transport.h>

static unsigned char command[31], flash[512], control[16384];
static unsigned int data_left, control_size, control_calls, writes, resets, opens;
static int broken_readback, transport_failure, wrong_tag, missing_device, mock_maskrom;
static int control_failure, storage_status;
static unsigned bulk_calls, erase_commands;

int rk_usb_scan(struct rockchip_device_info *devices, int capacity)
{
 assert(capacity > 0);
 if(missing_device) return 0;
 memset(devices, 0, sizeof(*devices));
 devices->bus = 1;
 strcpy(devices->port, "2.3");
 strcpy(devices->mode, "loader");
 return 1;
}
int rk_usb_open(struct openrockcli_ctx_t *ctx, uint8_t bus, const char *port)
{
 assert(bus == 1 && !strcmp(port, "2.3"));
 opens++;
 ctx->maskrom = mock_maskrom;
 ctx->pid = 0x350e;
 return 0;
}
void rk_usb_close(struct openrockcli_ctx_t *ctx) { (void)ctx; }
int rk_usb_has_bulk(const struct openrockcli_ctx_t *ctx) { (void)ctx; return 1; }
int rk_usb_control(struct openrockcli_ctx_t *ctx, uint16_t code, void *data, int size)
{
 (void)ctx;
 assert(code == 0x471 || code == 0x472);
 assert(size > 0 && size <= 4096);
 control_calls++;
 if(control_failure) return RK_USB_ERROR_TIMEOUT;
 assert(control_size + size <= sizeof(control));
 memcpy(control + control_size, data, size);
 control_size += size;
 return size;
}
int rk_usb_bulk(struct openrockcli_ctx_t *ctx, int input, void *data, int size, int *done)
{
 (void)ctx;
 bulk_calls++;
 if(transport_failure) return RK_USB_ERROR_TIMEOUT;
 *done = size;
 if(!input && !data_left) {
  assert(size == 31);
  memcpy(command, data, size);
  assert(!memcmp(command, "USBC", 4));
  data_left = get_unaligned_le32(command + 8);
  if(command[15] == 0xff) resets++;
  if(command[15] == 0x25) erase_commands++;
  return 0;
 }
 if(data_left) {
  if(command[15] == 0x1a) { assert(size == 512); *done = size = 11; }
  assert((unsigned)size == data_left);
  if(input) {
   memset(data, 0, size);
   switch(command[15]) {
   case 0xaa: assert(size == 8); ((unsigned char *)data)[0] = 8; break;
   case 0x2b: assert(size == 4); put_unaligned_le32(data, STORAGE_TYPE_EMMC); break;
   case 0x1a: assert(size == 11); put_unaligned_le32(data, 0x20000); break;
   case 0x01: assert(size == 5); break;
   case 0x14:
    assert(size == 512 && get_unaligned_be32(command + 17) == 0x10000);
    if(broken_readback) memset(data, 0xcc, size); else memcpy(data, flash, size);
    break;
   default: assert(0);
   }
  } else {
   assert(command[15] == 0x15 && size == 512);
   assert(get_unaligned_be32(command + 17) == 0x10000);
   memcpy(flash, data, size); writes++;
  }
  data_left = 0;
  return 0;
 }
 assert(input && size == 13);
 memset(data, 0, size);
 memcpy(data, "USBS", 4);
 memcpy((char *)data + 4, command + 4, 4);
 if(wrong_tag) ((char *)data)[4] ^= 1;
 if(command[15] == 0x2b && storage_status) ((unsigned char *)data)[12] = 1;
 return 0;
}

static void fixture(const char *path)
{
 unsigned char bytes[102 + 140 + 224 + 32] = {0};
 unsigned char *pkg = bytes + 102, *entry = pkg + 140;
 FILE *f;
 memcpy(bytes, "RKFW", 4);
 put_unaligned_le16(bytes + 4, 102);
 put_unaligned_le32(bytes + 33, 102);
 put_unaligned_le32(bytes + 37, sizeof(bytes) - 102);
 memcpy(pkg, "RKAF", 4);
 put_unaligned_le32(pkg + 136, 2);
 memcpy(entry, "bootloader", 10); memcpy(entry + 32, "loader.bin", 10);
 put_unaligned_le32(entry + 96, 364);
 put_unaligned_le32(entry + 108, 16);
 entry += 112;
 memcpy(entry, "boot", 4); memcpy(entry + 32, "boot.img", 8);
 put_unaligned_le32(entry + 96, 380);
 put_unaligned_le32(entry + 100, 0x10000);
 put_unaligned_le32(entry + 104, 1);
 put_unaligned_le32(entry + 108, 16);
 memset(bytes + sizeof(bytes) - 16, 0x5a, 16);
 f = rk_fopen(path, "wb"); assert(f);
 assert(fwrite(bytes, 1, sizeof(bytes), f) == sizeof(bytes));
 assert(!fclose(f));
}

int main(int argc, char **argv)
{
 struct openrockcli_ctx_t ctx = {0};
 unsigned char blob[8192] = {0};
 struct rockchip_flash_request request = {
  .mode="partition", .partitions="boot", .post_action="none",
  .bus="1", .port="2.3", .verify=1
 };
 assert(argc == 2);
 /* Explicit temporary path from the test runner. */
 request.firmware = argv[1];
 fixture(request.firmware);
 assert(rock_ready(&ctx));
 storage_status = 1; assert(rock_storage_read(&ctx) == STORAGE_TYPE_UNKNOWN); storage_status = 0;
 wrong_tag = 1; assert(!rock_ready(&ctx)); wrong_tag = 0;
 transport_failure = 1; assert(!rock_ready(&ctx)); transport_failure = 0;
 assert(rock_maskrom_upload_memory(&ctx, 0x471, blob, 8192, 0));
 assert(control_calls == 3 && control_size == 8194);
 assert(get_unaligned_be16(control + 8192) == crc16_sum(0xffff, blob, 8192));
 control_size = control_calls = 0;
 assert(rock_maskrom_upload_memory(&ctx, 0x472, blob, 4094, 0));
 assert(control_calls == 2 && control_size == 4097 && control[4096] == 0);
 control_failure = 1; control_calls = 0;
 assert(!rock_maskrom_upload_memory(&ctx, 0x471, blob, 8192, 0));
 assert(ctx.last_usb_error == RK_USB_ERROR_TIMEOUT);
 assert(control_calls == 1); control_failure = 0;
 assert(rockchip_backend_flash(&request));
 assert(writes == 1 && resets == 0);
 for(int i = 0; i < 512; i++) assert(flash[i] == (i < 16 ? 0x5a : 0));
 broken_readback = 1;
 assert(!rockchip_backend_flash(&request)); /* 0xcc must never silently disable verify. */
 broken_readback = 0;
 request.post_action = "reboot";
 assert(rockchip_backend_flash(&request) && resets == 1);
 unsigned previous_writes = writes, previous_opens = opens;
 request.port = "2.4";
 assert(!rockchip_backend_flash(&request));
 assert(writes == previous_writes && opens == previous_opens);
 request.port = "2.3"; missing_device = 1;
 assert(!rockchip_backend_flash(&request));
 assert(writes == previous_writes && opens == previous_opens);
 assert(!remove(request.firmware));
 return 0;
}
