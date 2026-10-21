/* Virtual Horizon RomFS storage backed directly by the extracted SSBU_ROMFS
 * directory. Only metadata tables are synthesized in memory; file data is read
 * on demand from the player's dump, avoiding a multi-gigabyte romfs.bin build.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct RDir RDir;
typedef struct RFile {
    char *name, *path;
    uint64_t size, data_off;
    uint32_t off;
    RDir *parent;
} RFile;
struct RDir {
    char *name, *path;
    RDir *parent;
    RDir **dirs;
    RFile **files;
    int ndirs, nfiles, cdirs, cfiles;
    uint32_t off;
};
typedef struct { RDir **dirs; RFile **files; int nd, nf, cd, cf; } Lists;
typedef struct { uint64_t start, size; char *path; } VFile;

static unsigned char *g_meta;
static uint64_t g_meta_size, g_size;
static VFile *g_files;
static int g_nfiles;
static CRITICAL_SECTION g_lock;
static volatile LONG g_lock_state;

static void lock_init(void) {
    if (InterlockedCompareExchange(&g_lock_state, 1, 0) == 0) {
        InitializeCriticalSection(&g_lock);
        InterlockedExchange(&g_lock_state, 2);
    }
    while (g_lock_state != 2) SwitchToThread();
}
static char *dupstr(const char *s) {
    size_t n = strlen(s) + 1; char *p = (char *)malloc(n); if (p) memcpy(p, s, n); return p;
}
static int cmp_dir(const void *a, const void *b) { return strcmp((*(RDir *const *)a)->name, (*(RDir *const *)b)->name); }
static int cmp_file(const void *a, const void *b) { return strcmp((*(RFile *const *)a)->name, (*(RFile *const *)b)->name); }

static RDir *scan_dir(const char *path, const char *name, RDir *parent, int depth) {
    if (depth > 48) return NULL;
    char pat[MAX_PATH * 4];
    snprintf(pat, sizeof pat, "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    RDir *d = (RDir *)calloc(1, sizeof *d);
    if (!d) { FindClose(h); return NULL; }
    d->name = dupstr(name); d->path = dupstr(path); d->parent = parent;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..") || fd.cFileName[0] == '.') continue;
        size_t need = strlen(path) + strlen(fd.cFileName) + 2;
        char *full = (char *)malloc(need);
        if (!full) continue;
        snprintf(full, need, "%s\\%s", path, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            RDir *c = scan_dir(full, fd.cFileName, d, depth + 1);
            free(full);
            if (!c) continue;
            if (d->ndirs == d->cdirs) {
                d->cdirs = d->cdirs ? d->cdirs * 2 : 8;
                d->dirs = (RDir **)realloc(d->dirs, sizeof *d->dirs * d->cdirs);
            }
            d->dirs[d->ndirs++] = c;
        } else {
            RFile *f = (RFile *)calloc(1, sizeof *f);
            if (!f) { free(full); continue; }
            f->name = dupstr(fd.cFileName); f->path = full; f->parent = d;
            f->size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            if (d->nfiles == d->cfiles) {
                d->cfiles = d->cfiles ? d->cfiles * 2 : 16;
                d->files = (RFile **)realloc(d->files, sizeof *d->files * d->cfiles);
            }
            d->files[d->nfiles++] = f;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (d->ndirs) qsort(d->dirs, d->ndirs, sizeof *d->dirs, cmp_dir);
    if (d->nfiles) qsort(d->files, d->nfiles, sizeof *d->files, cmp_file);
    return d;
}
static void flatten(RDir *d, Lists *l) {
    for (int i=0;i<d->nfiles;i++) {
        if (l->nf == l->cf) { l->cf = l->cf ? l->cf*2 : 256; l->files=(RFile**)realloc(l->files,sizeof *l->files*l->cf); }
        l->files[l->nf++] = d->files[i];
    }
    for (int i=0;i<d->ndirs;i++) {
        if (l->nd == l->cd) { l->cd = l->cd ? l->cd*2 : 64; l->dirs=(RDir**)realloc(l->dirs,sizeof *l->dirs*l->cd); }
        l->dirs[l->nd++] = d->dirs[i];
    }
    for (int i=0;i<d->ndirs;i++) flatten(d->dirs[i], l);
}
static void free_tree(RDir *d) {
    if (!d) return;
    for(int i=0;i<d->ndirs;i++) free_tree(d->dirs[i]);
    for(int i=0;i<d->nfiles;i++){ free(d->files[i]->name); free(d->files[i]->path); free(d->files[i]); }
    free(d->dirs); free(d->files); free(d->name); free(d->path); free(d);
}
static uint32_t hash_name(uint32_t parent,const char *name){
    uint32_t h=parent^123456789u; for(const unsigned char *p=(const unsigned char*)name;*p;p++) h=((h>>5)|(h<<27))^*p; return h;
}
static uint32_t buckets(uint32_t n){
    if(n<3)return 3; if(n<19)return n|1; uint32_t c=n;
    while(c%2==0||c%3==0||c%5==0||c%7==0||c%11==0||c%13==0||c%17==0)c++; return c;
}
static uint32_t a4(uint32_t x){return (x+3)&~3u;}
static uint64_t a16(uint64_t x){return (x+15)&~15ull;}
static void p32(unsigned char*p,uint32_t v){memcpy(p,&v,4);}
static void p64(unsigned char*p,uint64_t v){memcpy(p,&v,8);}

int scarlet_romfs_init(void) {
    lock_init(); EnterCriticalSection(&g_lock);
    if (g_meta) { LeaveCriticalSection(&g_lock); return 1; }
    const char *root = getenv("SSBU_ROMFS");
    if (!root || !*root) { fprintf(stderr,"Scarlet RomFS: SSBU_ROMFS is not set\n"); LeaveCriticalSection(&g_lock); return 0; }
    RDir *tree = scan_dir(root, "", NULL, 0);
    if (!tree) { fprintf(stderr,"Scarlet RomFS: cannot scan %s\n",root); LeaveCriticalSection(&g_lock); return 0; }
    Lists l={0};
    l.cd=64; l.dirs=(RDir**)malloc(sizeof *l.dirs*l.cd); l.dirs[l.nd++]=tree; flatten(tree,&l);
    if(!l.nf){fprintf(stderr,"Scarlet RomFS: no files in %s\n",root); free(l.dirs); free(l.files); free_tree(tree); LeaveCriticalSection(&g_lock); return 0;}

    uint32_t dsz=0,fsz=0;
    for(int i=0;i<l.nd;i++){l.dirs[i]->off=dsz; dsz+=0x18+a4((uint32_t)strlen(l.dirs[i]->name));}
    for(int i=0;i<l.nf;i++){l.files[i]->off=fsz; fsz+=0x20+a4((uint32_t)strlen(l.files[i]->name));}
    uint32_t nbd=buckets((uint32_t)l.nd), nbf=buckets((uint32_t)l.nf);
    uint64_t dh=0x50, dm=dh+4ull*nbd, fh=dm+dsz, fm=fh+4ull*nbf, data=a16(fm+fsz);
    uint64_t rel=0;
    for(int i=0;i<l.nf;i++){rel=a16(rel); l.files[i]->data_off=rel; rel+=l.files[i]->size;}
    g_meta_size=data; g_size=data+rel;
    g_meta=(unsigned char*)calloc(1,(size_t)data);
    uint32_t *db=(uint32_t*)(g_meta+dh), *fb=(uint32_t*)(g_meta+fh);
    for(uint32_t i=0;i<nbd;i++)db[i]=0xffffffffu; for(uint32_t i=0;i<nbf;i++)fb[i]=0xffffffffu;
    unsigned char *dt=g_meta+dm,*ft=g_meta+fm;
    for(int i=0;i<l.nd;i++){
        RDir*d=l.dirs[i]; unsigned char*e=dt+d->off; uint32_t par=d->parent?d->parent->off:0,nl=(uint32_t)strlen(d->name),sib=0xffffffffu;
        if(d->parent)for(int k=0;k+1<d->parent->ndirs;k++)if(d->parent->dirs[k]==d)sib=d->parent->dirs[k+1]->off;
        uint32_t h=hash_name(par,d->name)%nbd; p32(e,par);p32(e+4,sib);p32(e+8,d->ndirs?d->dirs[0]->off:0xffffffffu);
        p32(e+12,d->nfiles?d->files[0]->off:0xffffffffu);p32(e+16,db[h]);p32(e+20,nl);memcpy(e+0x18,d->name,nl);db[h]=d->off;
    }
    g_files=(VFile*)calloc((size_t)l.nf,sizeof *g_files); g_nfiles=l.nf;
    for(int i=0;i<l.nf;i++){
        RFile*f=l.files[i]; unsigned char*e=ft+f->off; uint32_t nl=(uint32_t)strlen(f->name),sib=0xffffffffu;
        for(int k=0;k+1<f->parent->nfiles;k++)if(f->parent->files[k]==f)sib=f->parent->files[k+1]->off;
        uint32_t h=hash_name(f->parent->off,f->name)%nbf; p32(e,f->parent->off);p32(e+4,sib);p64(e+8,f->data_off);p64(e+16,f->size);
        p32(e+24,fb[h]);p32(e+28,nl);memcpy(e+0x20,f->name,nl);fb[h]=f->off;
        g_files[i]=(VFile){data+f->data_off,f->size,dupstr(f->path)};
    }
    uint64_t hv[10]={0x50,dh,4ull*nbd,dm,dsz,fh,4ull*nbf,fm,fsz,data}; memcpy(g_meta,hv,sizeof hv);
    printf("Scarlet RomFS virtual: %d files, %llu MiB, no romfs.bin build\n",g_nfiles,(unsigned long long)(g_size>>20));
    free(l.dirs); free(l.files); free_tree(tree); LeaveCriticalSection(&g_lock); return 1;
}
uint64_t scarlet_romfs_size(void){ return g_size; }

static int find_file(uint64_t off) {
    int lo=0,hi=g_nfiles-1,best=-1;
    while(lo<=hi){int m=(lo+hi)/2;if(g_files[m].start<=off){best=m;lo=m+1;}else hi=m-1;} return best;
}
int scarlet_romfs_read(uint64_t off, void *dstv, uint64_t size) {
    if (!g_meta && !scarlet_romfs_init()) return 0;
    if (off > g_size || size > g_size-off) return 0;
    unsigned char *dst=(unsigned char*)dstv;
    while(size){
        if(off<g_meta_size){uint64_t n=g_meta_size-off;if(n>size)n=size;memcpy(dst,g_meta+off,(size_t)n);off+=n;dst+=n;size-=n;continue;}
        int i=find_file(off);
        if(i>=0 && off<g_files[i].start+g_files[i].size){
            VFile*f=&g_files[i];uint64_t pos=off-f->start,n=f->size-pos;if(n>size)n=size;
            FILE*fp=fopen(f->path,"rb"); if(!fp)return 0;
#ifdef _WIN32
            if(_fseeki64(fp,(long long)pos,SEEK_SET)!=0){fclose(fp);return 0;}
#else
            if(fseeko(fp,(off_t)pos,SEEK_SET)!=0){fclose(fp);return 0;}
#endif
            size_t got=fread(dst,1,(size_t)n,fp);fclose(fp);if(got!=(size_t)n)return 0;
            off+=n;dst+=n;size-=n;continue;
        }
        int next=i+1;uint64_t until=next<g_nfiles?g_files[next].start:g_size;uint64_t n=until-off;if(n>size)n=size;
        memset(dst,0,(size_t)n);off+=n;dst+=n;size-=n;
    }
    return 1;
}
