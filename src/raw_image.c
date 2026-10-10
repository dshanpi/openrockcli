/* Read-only whole-disk validation. Never opens USB or modifies the image. */
#include <rockchip_backend.h>
#include <rkimage.h>
#include <rkgpt.h>

static int raw_gpt(struct rkimage *image, const uint8_t *mbr)
{
 uint8_t h[512], backup[512], *entries=NULL, *copy=NULL;
 uint64_t sectors=image->file_size/512, first,last, table, backup_lba, bytes;
 uint32_t count,size,crc,header_size;int ok=0,used=0;
 (void)mbr;
 if(!rkimage_read(image,512,h,sizeof(h)) || memcmp(h,"EFI PART",8))return 0;
 header_size=get_unaligned_le32(h+12);crc=get_unaligned_le32(h+16);
 if(header_size<92 || header_size>512 || get_unaligned_le64(h+24)!=1)return 0;
 put_unaligned_le32(h+16,0);if(rkgpt_crc32(h,header_size)!=crc)return 0;
 backup_lba=get_unaligned_le64(h+32);first=get_unaligned_le64(h+40);last=get_unaligned_le64(h+48);
 table=get_unaligned_le64(h+72);count=get_unaligned_le32(h+80);size=get_unaligned_le32(h+84);
 if(!count || count>4096 || size<128 || size>1024 || size%128 || table<2 || first>last ||
    last>=backup_lba || backup_lba>=sectors)return 0;
 bytes=(uint64_t)count*size;
 if(table>=first || (bytes+511)/512>first-table)return 0;
 entries=malloc((size_t)bytes);copy=malloc((size_t)bytes);
 if(!entries || !copy || !rkimage_read(image,table*512,entries,(size_t)bytes) ||
    rkgpt_crc32(entries,(size_t)bytes)!=get_unaligned_le32(h+88))goto end;
 for(uint32_t i=0;i<count;i++) {
  uint8_t *e=entries+(size_t)i*size;uint64_t a,b;uint8_t empty[16]={0};
  if(!memcmp(e,empty,16))continue;
  used++;a=get_unaligned_le64(e+32);b=get_unaligned_le64(e+40);
  if(a<first || b>last || a>b)goto end;
  for(uint32_t j=0;j<i;j++) {
   uint8_t *p=entries+(size_t)j*size;
   if(memcmp(p,empty,16) && a<=get_unaligned_le64(p+40) && b>=get_unaligned_le64(p+32))goto end;
  }
 }
 if(!used || !rkimage_read(image,backup_lba*512,backup,512) || memcmp(backup,"EFI PART",8))goto end;
 header_size=get_unaligned_le32(backup+12);crc=get_unaligned_le32(backup+16);
 if(header_size<92 || header_size>512)goto end;
 put_unaligned_le32(backup+16,0);
 table=get_unaligned_le64(backup+72);
 if(rkgpt_crc32(backup,header_size)!=crc || get_unaligned_le64(backup+24)!=backup_lba ||
    get_unaligned_le64(backup+32)!=1 || get_unaligned_le64(backup+40)!=first ||
    get_unaligned_le64(backup+48)!=last || memcmp(h+56,backup+56,16) ||
    get_unaligned_le32(backup+80)!=count || get_unaligned_le32(backup+84)!=size ||
    get_unaligned_le32(backup+88)!=get_unaligned_le32(h+88) || table<=last || table>=backup_lba ||
    (bytes+511)/512>backup_lba-table || !rkimage_read(image,table*512,copy,(size_t)bytes) ||
    memcmp(copy,entries,(size_t)bytes))goto end;
 ok=1;
end:free(entries);free(copy);return ok;
}

int rockchip_raw_image(const char *path, struct rkimage *image)
{
 uint8_t mbr[512];int used=0,protective=0;
 if(!path || strlen(path)>=sizeof(image->path))return 0;
 memset(image,0,sizeof(*image));strcpy(image->path,path);
 if(rk_file_size(path,&image->file_size) || image->file_size<1024 || image->file_size%512 ||
    image->file_size/512>UINT32_MAX || !rkimage_read(image,0,mbr,512) ||
    !memcmp(mbr,"RKFW",4) || get_unaligned_le32(mbr)==0xed26ff3a || mbr[510]!=0x55 || mbr[511]!=0xaa)return 0;
 for(int i=0;i<4;i++) {
  uint8_t *e=mbr+446+i*16;uint64_t a=get_unaligned_le32(e+8),n=get_unaligned_le32(e+12);
  if(!e[4]) {if(a || n)return 0;continue;}
  if(e[0]!=0 && e[0]!=0x80)return 0;
  /* Extended partition chains are not accepted as complete primary layouts. */
  if(e[4]==5 || e[4]==0xf || e[4]==0x85 || !a || !n || a+n>image->file_size/512)return 0;
  used++;if(e[4]==0xee)protective++;
  for(int j=0;j<i;j++) {uint8_t *p=mbr+446+j*16;
   if(p[4] && a<(uint64_t)get_unaligned_le32(p+8)+get_unaligned_le32(p+12) &&
      a+n>get_unaligned_le32(p+8))return 0;
  }
 }
 return used && (!protective || (used==1 && raw_gpt(image,mbr)));
}

int rockchip_inspect_raw(const char *path, const char *loader)
{
 struct rkimage image;uint64_t size;struct rkloader_ctx_t *l;
 if(!rockchip_raw_image(path,&image)) {printf("Error: ROCKCHIP_RAW_FORMAT: expected a complete aligned MBR/GPT disk image\n");return 0;}
 if(!loader || rk_file_size(loader,&size) || !size || size>64U*1024*1024 || !(l=rkloader_ctx_alloc(loader))) {
  printf("Error: ROCKCHIP_RAW_LOADER: invalid RAM Loader (maximum 64 MiB)\n");return 0;
 }
 int has471=0,has472=0;
 for(int i=0;i<l->nentry;i++){has471|=l->entry[i]->type==RKLOADER_ENTRY_471;has472|=l->entry[i]->type==RKLOADER_ENTRY_472;}
 rkloader_ctx_free(l);
 if(!has471 || !has472){printf("Error: ROCKCHIP_RAW_LOADER: missing RAM download entries\n");return 0;}
 printf("Raw eMMC image: %llu bytes; RAM Loader valid; board compatibility requires a matched image and Loader.\n",(unsigned long long)image.file_size);
 return 1;
}
