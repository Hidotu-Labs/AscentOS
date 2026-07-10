#include "ext4_dir_index.h"
#include "ext4_extent.h"
#include "fs/ext2/ext2_internal.h"
#include "lib/string.h"
#include "mm/heap.h"
#include <stdint.h>

#define F(x,y,z) ((z)^((x)&((y)^(z))))
#define G(x,y,z) (((x)&(y))+(((x)^(y))&(z)))
#define H(x,y,z) ((x)^(y)^(z))
#define ROL(x,s) (((x)<<(s))|((x)>>(32-(s))))
#define ROUND(f,a,b,c,d,x,s) (a+=f(b,c,d)+(x),a=ROL(a,s))
#define K2 013240474631u
#define K3 015666365641u

typedef struct { uint32_t zero; uint8_t version; uint8_t length; uint8_t levels; uint8_t flags; } __attribute__((packed)) dx_info_t;
typedef struct { uint16_t limit; uint16_t count; } __attribute__((packed)) dx_cl_t;
typedef struct { uint32_t hash; uint32_t block; } __attribute__((packed)) dx_entry_t;
typedef struct { uint32_t hash; uint32_t inode; uint16_t size; uint8_t name_len; uint8_t type; char name[256]; } dx_item_t;

static void hashbuf(const char *name, int len, uint32_t *out, int words, int uns) {
  uint32_t pad=(uint32_t)len|((uint32_t)len<<8); pad|=pad<<16;
  uint32_t value=pad; int left=words;
  if(len>words*4) len=words*4;
  for(int i=0;i<len;i++) {
    int ch=uns?(int)(uint8_t)name[i]:(int)(int8_t)name[i];
    value=(uint32_t)ch+(value<<8);
    if((i&3)==3) { *out++=value; value=pad; left--; }
  }
  if(--left>=0) *out++=value;
  while(--left>=0) *out++=pad;
}

static void half_md4(uint32_t s[4],const uint32_t x[8]) {
  uint32_t a=s[0],b=s[1],c=s[2],d=s[3];
  ROUND(F,a,b,c,d,x[0],3); ROUND(F,d,a,b,c,x[1],7); ROUND(F,c,d,a,b,x[2],11); ROUND(F,b,c,d,a,x[3],19);
  ROUND(F,a,b,c,d,x[4],3); ROUND(F,d,a,b,c,x[5],7); ROUND(F,c,d,a,b,x[6],11); ROUND(F,b,c,d,a,x[7],19);
  ROUND(G,a,b,c,d,x[1]+K2,3); ROUND(G,d,a,b,c,x[3]+K2,5); ROUND(G,c,d,a,b,x[5]+K2,9); ROUND(G,b,c,d,a,x[7]+K2,13);
  ROUND(G,a,b,c,d,x[0]+K2,3); ROUND(G,d,a,b,c,x[2]+K2,5); ROUND(G,c,d,a,b,x[4]+K2,9); ROUND(G,b,c,d,a,x[6]+K2,13);
  ROUND(H,a,b,c,d,x[3]+K3,3); ROUND(H,d,a,b,c,x[7]+K3,9); ROUND(H,c,d,a,b,x[2]+K3,11); ROUND(H,b,c,d,a,x[6]+K3,15);
  ROUND(H,a,b,c,d,x[1]+K3,3); ROUND(H,d,a,b,c,x[5]+K3,9); ROUND(H,c,d,a,b,x[0]+K3,11); ROUND(H,b,c,d,a,x[4]+K3,15);
  s[0]+=a; s[1]+=b; s[2]+=c; s[3]+=d;
}

static int dirhash(ext2_mount_t *mnt,uint8_t version,const char *name,int len,uint32_t *hash) {
  if(version!=1&&version!=4) return -1;
  uint32_t s[4]={0x67452301,0xefcdab89,0x98badcfe,0x10325476};
  if(mnt->sb.s_hash_seed[0]||mnt->sb.s_hash_seed[1]||mnt->sb.s_hash_seed[2]||mnt->sb.s_hash_seed[3]) memcpy(s,mnt->sb.s_hash_seed,sizeof(s));
  const char *p=name; int left=len;
  while(left>0) { uint32_t x[8]; hashbuf(p,left,x,8,version==4); half_md4(s,x); p+=32; left-=32; }
  *hash=s[1]&0xfffffffeu;
  if(*hash==0xfffffffeu) *hash=0xfffffffcu;
  return 0;
}

static int leaf_insert(uint8_t *buf,uint32_t bs,uint32_t inode,const char *name,uint8_t nl,uint8_t type) {
  uint32_t need=(8u+nl+3u)&0xfffffffcu;
  for(uint32_t off=0;off<bs;) {
    ext2_dirent_t *e=(ext2_dirent_t *)(buf+off);
    if(e->rec_len<8||off+e->rec_len>bs) return -1;
    if(!e->inode&&e->rec_len>=need) {
      uint16_t rec=e->rec_len; memset(e,0,rec); e->inode=inode; e->rec_len=rec; e->name_len=nl; e->file_type=type; memcpy(e->name,name,nl); return 0;
    }
    uint32_t used=(8u+e->name_len+3u)&0xfffffffcu;
    if(e->rec_len>=used+need) {
      uint16_t rec=e->rec_len; e->rec_len=used; ext2_dirent_t *n=(ext2_dirent_t *)(buf+off+used);
      memset(n,0,rec-used); n->inode=inode; n->rec_len=rec-used; n->name_len=nl; n->file_type=type; memcpy(n->name,name,nl); return 0;
    }
    off+=e->rec_len;
  }
  return 1;
}

static int collect(ext2_mount_t *mnt,uint8_t version,uint8_t *buf,uint32_t bs,dx_item_t *items,uint32_t cap,uint32_t *count) {
  for(uint32_t off=0;off<bs;) {
    ext2_dirent_t *e=(ext2_dirent_t *)(buf+off);
    if(e->rec_len<8||off+e->rec_len>bs) return -1;
    if(e->inode) {
      if(*count>=cap) return -1;
      dx_item_t *i=&items[(*count)++];
      if(dirhash(mnt,version,e->name,e->name_len,&i->hash)) return -1;
      i->inode=e->inode; i->name_len=e->name_len; i->type=e->file_type; i->size=(8u+e->name_len+3u)&0xfffffffcu; memcpy(i->name,e->name,e->name_len);
    }
    off+=e->rec_len;
  }
  return 0;
}

static void sort_items(dx_item_t *items,uint32_t count) {
  for(uint32_t i=1;i<count;i++) { dx_item_t v=items[i]; uint32_t j=i; while(j&&items[j-1].hash>v.hash) { items[j]=items[j-1]; j--; } items[j]=v; }
}

static int pack(uint8_t *buf,uint32_t bs,dx_item_t *items,uint32_t first,uint32_t end) {
  if(first==end) return -1;
  memset(buf,0,bs);
  uint32_t off=0;
  for(uint32_t i=first;i<end;i++) {
    uint32_t rec=i+1==end?bs-off:items[i].size;
    if(rec<items[i].size||off+rec>bs) return -1;
    ext2_dirent_t *e=(ext2_dirent_t *)(buf+off); e->inode=items[i].inode; e->rec_len=rec; e->name_len=items[i].name_len; e->file_type=items[i].type; memcpy(e->name,items[i].name,e->name_len); off+=rec;
  }
  return 0;
}

int ext4_dx_add_entry(ext2_mount_t *mnt,uint32_t ino,ext2_inode_t *inode,uint32_t child,const char *name,uint8_t type) {
  uint32_t nl=strlen(name); if(!nl||nl>255||!(inode->i_flags&EXT2_INDEX_FL)) return -1;
  uint8_t *root=kmalloc(mnt->block_size),*leaf=kmalloc(mnt->block_size),*right=kmalloc(mnt->block_size);
  uint32_t cap=mnt->block_size/8+1; dx_item_t *items=kmalloc(cap*sizeof(*items)); int result=-1;
  if(!root||!leaf||!right||!items) goto out;
  uint32_t root_phys=ext2_get_block_num(mnt,inode,0); if(!root_phys||ext2_read_block(mnt,root_phys,root)) goto out;
  dx_info_t *info=(dx_info_t *)(root+24); dx_cl_t *cl=(dx_cl_t *)(root+32); dx_entry_t *entries=(dx_entry_t *)(root+32);
  if(info->zero||info->length!=8||info->levels||!cl->count||cl->count>cl->limit||32u+(uint32_t)cl->limit*8u>mnt->block_size) goto out;
  uint32_t hash; if(dirhash(mnt,info->version,name,nl,&hash)) goto out;
  uint16_t at=0; for(uint16_t i=1;i<cl->count;i++) { if(hash<entries[i].hash) break; at=i; }
  uint32_t leaf_log=entries[at].block&0x0fffffffu; uint32_t leaf_phys=ext2_get_block_num(mnt,inode,leaf_log);
  if(!leaf_phys||ext2_read_block(mnt,leaf_phys,leaf)) goto out;
  int inserted=leaf_insert(leaf,mnt->block_size,child,name,nl,type);
  if(inserted==0) { result=ext3_journal_block(mnt,leaf_phys,leaf); goto out; }
  if(inserted<0||cl->count>=cl->limit) goto out;
  uint32_t count=0; if(collect(mnt,info->version,leaf,mnt->block_size,items,cap,&count)||count>=cap) goto out;
  dx_item_t *n=&items[count++]; n->hash=hash; n->inode=child; n->name_len=nl; n->type=type; n->size=(8u+nl+3u)&0xfffffffcu; memcpy(n->name,name,nl); sort_items(items,count);
  uint32_t total=0; for(uint32_t i=0;i<count;i++) total+=items[i].size;
  uint32_t split=1,bytes=items[0].size; while(split+1<count&&bytes+items[split].size<total/2) { bytes+=items[split].size; split++; }
  if(pack(leaf,mnt->block_size,items,0,split)||pack(right,mnt->block_size,items,split,count)) goto out;
  uint32_t new_log=inode->i_size/mnt->block_size; uint64_t new_phys;
  if(ext4_alloc_extent(mnt,inode,ino,new_log,1,&new_phys)||new_phys>UINT32_MAX) goto out;
  uint32_t boundary=items[split].hash; if(items[split-1].hash==boundary) boundary|=1;
  for(uint16_t i=cl->count;i>at+1;i--) entries[i]=entries[i-1];
  entries[at+1].hash=boundary; entries[at+1].block=new_log; cl->count++;
  inode->i_size+=mnt->block_size; inode->i_blocks+=mnt->block_size/512;
  result=ext3_journal_block(mnt,leaf_phys,leaf);
  if(!result) result=ext3_journal_block(mnt,(uint32_t)new_phys,right);
  if(!result) result=ext3_journal_block(mnt,root_phys,root);
  if(!result) result=ext2_write_inode(mnt,ino,inode);
out:
  if(root) kfree(root);
  if(leaf) kfree(leaf);
  if(right) kfree(right);
  if(items) kfree(items);
  return result;
}
