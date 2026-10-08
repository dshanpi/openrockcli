/* Full update.img -> IDB/GPT/partition workflow over memory, never hardware. */
#include <rockchip_backend.h>
#include <rkimage.h>
#include <rkgpt.h>
static unsigned char disk[0x20000 * 512], command[31];
static unsigned left, position, erases, writes, resets, reset_mode, bulk_calls, controls, opens;
static int failure, fail_at, start_maskrom, downloaded, storage = STORAGE_TYPE_EMMC, reset_failure, info_size = 11, corrupt_lba = -1, storage_failure, failed;
static unsigned char loader[4096];
static size_t loader_size;
static unsigned disk_sectors = 0x20000, delayed_maskrom, maskrom_bulk, read_blocked, read_failure;
static unsigned backup_reads, corrupt_after_read, backup_read_failure, ordinary_mismatch;
static char event_log[8192];
static unsigned capabilities = 8, capability_calls, parity_writes, bad_parity_lba = UINT32_MAX;
static int capability_failure, capability_wrong_tag, parity_wrong_tag, parity_residue;
static unsigned mock_checksum(const unsigned char *p, unsigned n) {
 unsigned sum=0x55aa;
 for(unsigned i=0;i<n;i+=4) sum += (unsigned)p[i] | ((unsigned)p[i+1]<<8) | ((unsigned)p[i+2]<<16) | ((unsigned)p[i+3]<<24);
 return sum;
}
/* Map only the tail of a 58.2 GiB disk into the existing in-memory fixture. */
static unsigned map_lba(unsigned lba) {
 if(disk_sectors > 0x20000 && lba >= disk_sectors - RKGPT_BACKUP_SECTORS)
  return 0x20000 - (disk_sectors - lba);
 return lba;
}
#define CHECK(x) do { if(!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); failed++; } } while(0)
int rk_usb_scan(struct rockchip_device_info *d, int n) {
 assert(n); memset(d,0,sizeof(*d)); d->bus=0; strcpy(d->port,"PCIROOT(0)#USBROOT(0)#USB(1)"); strcpy(d->mode,"loader"); return 1;
}
int rk_usb_open(struct openrockcli_ctx_t *c,uint8_t bus,const char *port) {
 assert(!bus && !strcmp(port,"PCIROOT(0)#USBROOT(0)#USB(1)")); c->pid=0x350b; c->maskrom=start_maskrom && (!downloaded || opens<=delayed_maskrom); opens++; return 0;
}
void rk_usb_close(struct openrockcli_ctx_t *c) { (void)c; }
int rk_usb_has_bulk(const struct openrockcli_ctx_t *c) {return !c->maskrom;}
int rk_usb_control(struct openrockcli_ctx_t *c,uint16_t code,void *d,int n) { (void)c;(void)d;controls++;if(code==0x472)downloaded=1;return n; }
int rk_usb_bulk(struct openrockcli_ctx_t *c,int input,void *p,int n,int *done) {
 bulk_calls++; *done=0;
 if(c->maskrom) {maskrom_bulk++;return RK_USB_ERROR_NOT_SUPPORTED;}
 if(failure || (fail_at && bulk_calls==(unsigned)fail_at)) return RK_USB_ERROR_NO_DEVICE;
 *done=n;
 if(!input && !left) {
  assert(n==31);memcpy(command,p,31);assert(!memcmp(p,"USBC",4));
  left=get_unaligned_le32(command+8);position=0;
  if(command[15]==0x14 && get_unaligned_be32(command+17)==disk_sectors-RKGPT_BACKUP_SECTORS) backup_reads++;
  if(command[15]==0xaa) capability_calls++;
  if(command[15]==0x25) {erases++;unsigned lba=get_unaligned_be32(command+17),cnt=get_unaligned_be16(command+22);assert((uint64_t)lba+cnt<=0x20000);memset(disk+lba*512,0xff,cnt*512);}
  if(command[15]==0xff) {resets++;reset_mode=command[16];}
  return 0;
 }
 if(left) {
  unsigned lba=get_unaligned_be32(command+17), mapped=map_lba(lba);
  if(command[15]==0x1a) {
   /* Modern Loader returns a longer descriptor than the CBW's 11 bytes. */
   if(n<info_size) return RK_USB_ERROR_OVERFLOW;
   memset(p,0,info_size);put_unaligned_le32(p,disk_sectors);*done=info_size;left=0;return 0;
  }
  assert((unsigned)n<=left);
  if(input) {
   memset(p,0,n);
   if(command[15]==0xaa) {assert(n==8);put_unaligned_le32(p,capabilities);}
   else if(command[15]==0x2b) put_unaligned_le32(p,storage);
   else if(command[15]==0x14) {
    if(read_failure && lba>=0x10000) {*done=0;return RK_USB_ERROR_NO_DEVICE;}
    if(backup_read_failure && backup_reads>=2 && lba==disk_sectors-RKGPT_BACKUP_SECTORS) {*done=0;return RK_USB_ERROR_NO_DEVICE;}
    assert((uint64_t)mapped*512+position+n<=sizeof(disk));memcpy(p,disk+(size_t)mapped*512+position,n);
    if(((int)lba==corrupt_lba && writes) || (read_blocked && lba>=read_blocked) ||
       (corrupt_after_read && backup_reads>=corrupt_after_read && lba==disk_sectors-RKGPT_BACKUP_SECTORS)) memset(p,0xcc,n);
    if(ordinary_mismatch && backup_reads>=2 && lba==disk_sectors-RKGPT_BACKUP_SECTORS) ((unsigned char *)p)[0]^=1;
   }
   else assert(command[15]==0x01);
  } else {assert(command[15]==0x15);assert((uint64_t)mapped*512+position+n<=sizeof(disk));memcpy(disk+(size_t)mapped*512+position,p,n);writes++;}
  left-=n;position+=n;return 0;
 }
 assert(input && n==13);memset(p,0,n);memcpy(p,"USBS",4);memcpy((char*)p+4,command+4,4);
 if(command[15]==0xaa) {if(capability_failure)((unsigned char*)p)[12]=1;if(capability_wrong_tag)((unsigned char*)p)[4]^=1;}
 if(command[15]==0x15 && command[14]==16 && get_unaligned_le32(command+24)) {
  unsigned lba=get_unaligned_be32(command+17), bytes=get_unaligned_le32(command+8);
  parity_writes++;
  if(parity_wrong_tag)((unsigned char*)p)[4]^=1;
  if(parity_residue)put_unaligned_le32((unsigned char*)p+8,512);
  assert(bytes<=128*512 && get_unaligned_be16(command+22)*512==bytes);
  assert(!command[16] && !command[21] && !command[28] && !command[29] && !command[30]);
  if(lba==bad_parity_lba)disk[(size_t)map_lba(lba)*512]^=1;
  if(mock_checksum(disk+(size_t)map_lba(lba)*512,bytes)!=get_unaligned_le32(command+24))((unsigned char*)p)[12]=2;
 }
 if(reset_failure && command[15]==0xff) ((unsigned char*)p)[12]=1;
 if(storage_failure && command[15]==0x2b) ((unsigned char*)p)[12]=1;
 return 0;
}
static void make_loader(void) {
 memset(loader,0,sizeof(loader)); struct rkloader_header_t *h=(void*)loader;
 h->tag=cpu_to_le32(0x2052444c);h->size=cpu_to_le16(sizeof(*h));h->rc4_flag=1;
 h->code471_num=1;h->code471_offset=cpu_to_le32(sizeof(*h));h->code471_size=sizeof(struct rkloader_entry_t);
 h->code472_num=1;h->code472_offset=cpu_to_le32(sizeof(*h)+sizeof(struct rkloader_entry_t));h->code472_size=sizeof(struct rkloader_entry_t);
 h->loader_num=1;h->loader_offset=cpu_to_le32(sizeof(*h)+2*sizeof(struct rkloader_entry_t));h->loader_size=sizeof(struct rkloader_entry_t);
 size_t pos=sizeof(*h)+3*sizeof(struct rkloader_entry_t);
 for(int i=0;i<3;i++) {
  struct rkloader_entry_t *e=(void*)(loader+sizeof(*h)+i*sizeof(*e));e->size=sizeof(*e);e->type=1U<<i;
  const char *name=i==2?"FlashHead":"Download";for(unsigned j=0;j<strlen(name);j++)e->name[j]=cpu_to_le16(name[j]);
  unsigned len=i==2?512:16;e->data_offset=cpu_to_le32(pos);e->data_size=cpu_to_le32(len);memset(loader+pos,0xa5,len);pos+=len;
 }
 loader_size=pos+4;put_unaligned_le32(loader+loader_size-4,crc32_sum(0,loader,loader_size-4));
}
static void image_fixture(const char *path,int variant) {
 unsigned char bytes[8192]={0};unsigned char *pkg=bytes+102,*entry=pkg+140;
 const char *names[]={"bootloader","parameter","boot"};
 const char *parameter="CMDLINE: mtdparts=rk29xxnand:0x10@0x10000(boot)\n";
 make_loader(); if(variant==2) loader[0]=0;
 if(variant==3) parameter="invalid parameter";
 size_t pos=140+3*112;
 for(int i=0;i<3;i++,entry+=112) {
  strcpy((char*)entry,names[i]);strcpy((char*)entry+32,names[i]);put_unaligned_le32(entry+96,pos);
  size_t len=i==0?loader_size:i==1?strlen(parameter):variant==4?1024:16;
  put_unaligned_le32(entry+100,i==2?(variant==1?0x20000:0x10000):0);put_unaligned_le32(entry+104,1);put_unaligned_le32(entry+108,len);
  if(i==0)memcpy(pkg+pos,loader,len);else if(i==1)memcpy(pkg+pos,parameter,len);else memset(pkg+pos,0x5a,len);
  pos+=len;
 }
 memcpy(bytes,"RKFW",4);put_unaligned_le16(bytes+4,102);put_unaligned_le32(bytes+33,102);put_unaligned_le32(bytes+37,pos);
 memcpy(pkg,"RKAF",4);put_unaligned_le32(pkg+136,3);
 FILE *f=rk_fopen(path,"wb");assert(f);assert(fwrite(bytes,1,102+pos,f)==102+pos);assert(!fclose(f));
}
static void clear_counts(void) {erases=writes=resets=bulk_calls=controls=opens=left=0;failure=storage_failure=fail_at=reset_failure=0;start_maskrom=downloaded=0;storage=STORAGE_TYPE_EMMC;corrupt_lba=-1;disk_sectors=0x20000;delayed_maskrom=maskrom_bulk=read_blocked=read_failure=backup_reads=corrupt_after_read=backup_read_failure=ordinary_mismatch=0;event_log[0]=0;capabilities=8;capability_calls=parity_writes=capability_failure=capability_wrong_tag=0;bad_parity_lba=UINT32_MAX;parity_wrong_tag=parity_residue=0;}
static int saw_verify_error;
static void event(const struct rockchip_backend_event *e,void *u) {(void)u;if(e->text) {if(strstr(e->text,"ROCKCHIP_VERIFY_FAILED"))saw_verify_error=1;size_t used=strlen(event_log);snprintf(event_log+used,sizeof(event_log)-used,"%s\n",e->text);}}
int main(int argc,char **argv) {
 assert(argc==2);struct rockchip_flash_request r={.firmware=argv[1],.mode="full_erase",.post_action="none",.bus="0",.port="PCIROOT(0)#USBROOT(0)#USB(1)",.verify=1};
 struct openrockcli_ctx_t ctx={0};struct flash_info_t info;
 /* Wire contract: unaligned input, chunk boundary, CSW failure, zero sentinel. */
 unsigned char *allocation=calloc(1,129*512+1), *data=allocation+1;CHECK(allocation);
 data[0]=1;data[128*512]=2;
 clear_counts();CHECK(rock_flash_write_lba_parity(&ctx,0x10000,129,data));
 CHECK(parity_writes==2 && writes==2 && command[14]==16);
 CHECK(get_unaligned_be32(command+17)==0x10080 && get_unaligned_le32(command+24)==0x55ac);
 CHECK(get_unaligned_le32(command+8)==512 && command[12]==0 && command[13]==0);
 for(int i=0;i<3;i++) {
  clear_counts();bad_parity_lba=i==0?0x10000:UINT32_MAX;parity_wrong_tag=i==1;parity_residue=i==2;
  CHECK(!rock_flash_write_lba_parity(&ctx,0x10000,129,data));CHECK(writes==1 && bulk_calls==3 && !resets);
 }
 /* Zero in the SECOND chunk must also reject before the first CBW. */
 clear_counts();memset(data,0,129*512);put_unaligned_le32(data+128*512,0xffffaa56U);
 CHECK(!rock_flash_write_lba_parity(&ctx,0x10000,129,data));CHECK(!writes && !bulk_calls);
 clear_counts();CHECK(!rock_flash_write_lba_parity(&ctx,UINT32_MAX,2,data));CHECK(!bulk_calls);
 clear_counts();CHECK(rock_flash_write_lba(&ctx,0x10000,1,data));CHECK(command[14]==16 && !get_unaligned_le32(command+24));
 free(allocation);
 for(int n=11;n<=512;n+=501) {clear_counts();info_size=n;CHECK(rock_flash_detect(&ctx,&info));CHECK(info.sector_total==0x20000);}
 info_size=11;
 for(int variant=1;variant<=3;variant++) {clear_counts();image_fixture(r.firmware,variant);CHECK(!rockchip_backend_flash(&r));CHECK(erases==0 && writes==0 && resets==0);}
 clear_counts();image_fixture(r.firmware,0);CHECK(rockchip_backend_flash(&r));CHECK(erases>0 && writes>0 && resets==0);CHECK(!memcmp(disk+512,"EFI PART",8));CHECK(disk[0x10000*512]==0x5a);
 for(int i=0;i<3;i++) {clear_counts();corrupt_lba=i==0?64:i==1?0:0x20000-RKGPT_BACKUP_SECTORS;saw_verify_error=0;CHECK(!rockchip_backend_flash_with_cb(&r,event,NULL));CHECK(saw_verify_error);CHECK(resets==0);}
 clear_counts();failure=1;CHECK(!rockchip_backend_flash(&r));CHECK(bulk_calls==1 && !writes && !erases);
 clear_counts();storage_failure=1;CHECK(!rockchip_backend_flash(&r));CHECK(!writes && !erases);
 clear_counts();r.post_action="poweroff";CHECK(rockchip_backend_flash(&r));CHECK(resets==1 && reset_mode==2);
 clear_counts();r.partitions="missing";CHECK(!rockchip_backend_flash(&r));CHECK(!writes && !erases && !resets);
 make_loader();struct rkloader_ctx_t *l=rkloader_ctx_alloc_data(loader,loader_size);CHECK(l && l->idblen==2048);rkloader_ctx_free(l);
 r.partitions=NULL;r.post_action="none";
 clear_counts();start_maskrom=1;CHECK(rockchip_backend_flash(&r));CHECK(controls==2 && opens==2 && downloaded && !resets);
 clear_counts();start_maskrom=1;delayed_maskrom=11;CHECK(rockchip_backend_flash(&r));CHECK(controls==2 && opens==13 && !maskrom_bulk && !resets);
 /* Read restriction is visible before destructive commands, at real reported capacity. */
 clear_counts();disk_sectors=122142720;read_blocked=0x10000;r.mode="partition";
 CHECK(!rockchip_backend_flash_with_cb(&r,event,NULL));CHECK(!writes && !erases && !resets);
 CHECK(strstr(event_log,"ROCKCHIP_VERIFY_UNAVAILABLE") && strstr(event_log,"before erase") && strstr(event_log,"all-0xcc"));
 clear_counts();read_failure=1;CHECK(!rockchip_backend_flash_with_cb(&r,event,NULL));CHECK(!writes && !erases && !resets);
 CHECK(strstr(event_log,"reason=read-failed"));
 clear_counts();image_fixture(r.firmware,4);r.partitions="boot";read_blocked=0x10001;
 CHECK(!rockchip_backend_flash_with_cb(&r,event,NULL));CHECK(!writes && !erases && !resets);
 CHECK(strstr(event_log,"lba=0x10001") && strstr(event_log,"ROCKCHIP_VERIFY_UNAVAILABLE"));
 /* Only an explicit verify=false skips readback; never changed automatically. */
 clear_counts();read_blocked=0x10000;r.verify=0;
 CHECK(rockchip_backend_flash_with_cb(&r,event,NULL));CHECK(writes && !backup_reads);
 CHECK(strstr(event_log,"Verification: not requested"));
 image_fixture(r.firmware,0);r.partitions=NULL;r.verify=1;
 /* Full direct-LBA readback works at that same 58.2 GiB geometry. */
 clear_counts();disk_sectors=122142720;CHECK(rockchip_backend_flash(&r));CHECK(writes && !erases && !resets);
 CHECK(get_unaligned_le64(disk+512+32)==122142719 && get_unaligned_le64(disk+sizeof(disk)-512+24)==122142719);
 /* A new failure after a successful precheck is still fatal, with exact context. */
 clear_counts();corrupt_after_read=2;CHECK(!rockchip_backend_flash_with_cb(&r,event,NULL));CHECK(writes && !resets);
 CHECK(strstr(event_log,"ROCKCHIP_VERIFY_FAILED backup GPT") && strstr(event_log,"reason=all-0xcc") && strstr(event_log,"first_difference="));
 clear_counts();ordinary_mismatch=1;CHECK(!rockchip_backend_flash_with_cb(&r,event,NULL));CHECK(writes && !resets);
 CHECK(strstr(event_log,"ROCKCHIP_VERIFY_FAILED backup GPT reason=data-mismatch") && strstr(event_log,"first_difference=0"));
 clear_counts();backup_read_failure=1;CHECK(!rockchip_backend_flash_with_cb(&r,event,NULL));CHECK(writes && !resets);
 CHECK(strstr(event_log,"ROCKCHIP_VERIFY_FAILED backup GPT reason=read-failed") && !strstr(event_log,"first_difference="));
 /* A restricted Loader can verify media on-device even when host reads are blocked. */
 clear_counts();disk_sectors=122142720;capabilities=0x405;read_blocked=0x10000;
 CHECK(rockchip_backend_flash_with_cb(&r,event,NULL));CHECK(writes && parity_writes==writes && !backup_reads && capability_calls==1 && !resets);
 CHECK(strstr(event_log,"Verification method: device-checksum"));
 CHECK(get_unaligned_le64(disk+sizeof(disk)-512+24)==122142719);
 for(int i=0;i<3;i++) {
  clear_counts();capabilities=0x405;read_blocked=0x10000;bad_parity_lba=i==0?64:i==1?disk_sectors-RKGPT_BACKUP_SECTORS:0x10000;
  CHECK(!rockchip_backend_flash_with_cb(&r,event,NULL));CHECK(writes && !resets && parity_writes==writes);
  CHECK(strstr(event_log,"ROCKCHIP_VERIFY_FAILED") && strstr(event_log,"device-checksum"));
 }
 /* No capability, malformed capability CSW and command failure all stop before erase. */
 for(int i=0;i<3;i++) {
  clear_counts();capabilities=0;capability_failure=i==1;capability_wrong_tag=i==2;
  CHECK(!rockchip_backend_flash_with_cb(&r,event,NULL));CHECK(!writes && !erases && !resets);
  CHECK(strstr(event_log,"ROCKCHIP_VERIFY_UNAVAILABLE"));
 }
 clear_counts();capabilities=0x40d;corrupt_after_read=2;
 CHECK(!rockchip_backend_flash_with_cb(&r,event,NULL));CHECK(!parity_writes && writes && !resets);
 CHECK(strstr(event_log,"Verification method: host-readback") && strstr(event_log,"ROCKCHIP_VERIFY_FAILED backup GPT"));
 r.mode="full_erase";
 for(int medium=0;medium<2;medium++) {
  clear_counts();storage=medium?STORAGE_TYPE_SPINAND:STORAGE_TYPE_SPINOR;
  CHECK(rockchip_backend_flash(&r));CHECK(!memcmp(disk+512,"EFI PART",8));
  /* First byte of 0xa5 encrypted with the documented Rockchip RC4 key is 0xcb. */
  CHECK(disk[(medium?256:128)*512]==0xcb);
  if(medium)CHECK(disk[768*512]==0xcb);
 }
 for(int step=1;step<=40;step+=7) {
  clear_counts();fail_at=step;CHECK(!rockchip_backend_flash(&r));CHECK(bulk_calls==(unsigned)step && !resets);
 }
 clear_counts();reset_failure=1;r.post_action="poweroff";CHECK(!rockchip_backend_flash(&r));CHECK(resets==1);
 for(int v=0;v<7;v++) {
  make_loader(); struct rkloader_header_t *h=(void*)loader; struct rkloader_entry_t *e=(void*)(loader+le32_to_cpu(h->loader_offset));
  if(v==0)h->loader_num=33;
  if(v==1)h->loader_offset=cpu_to_le32(UINT32_MAX);
  if(v==2)e->data_offset=cpu_to_le32(UINT32_MAX);
  if(v==3)e->data_size=0;
  if(v==4)e->type=RKLOADER_ENTRY_471;
  if(v==5)h->tag=cpu_to_le32(0x544f4f42); /* Old IDB needs both FlashData and FlashBoot. */
  put_unaligned_le32(loader+loader_size-4,crc32_sum(0,loader,loader_size-4));
  if(v==6)loader[loader_size-1]^=1;
  l=rkloader_ctx_alloc_data(loader,loader_size);CHECK(!l);rkloader_ctx_free(l);
 }
 /* Guarded UTF-16 input: the length is bytes, not UTF-16 code units. */
 unsigned char name[]={ 'A',0,'B',0 };char out[8];memset(out,'X',sizeof(out));loader_wide2str(out,name,sizeof(name));CHECK(!strcmp(out,"AB") && out[3]=='X');
 CHECK(!remove(r.firmware));fprintf(stderr,"audit failures=%d\n",failed);return failed?1:0;
}
