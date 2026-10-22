#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include "../runtime/nvn_sdk.h"
void report_and_exit(int code) { exit(code); }
int main(void) {
 NvnSdkTexBuilder b={0}; b.target=1;b.format=0x25;b.width=16;b.height=16;b.depth=1;b.levels=1;
 assert(nvn_sdk_texture_storage_alignment(0,&b)==512);
 assert(nvn_sdk_texture_storage_size(0,&b)==1024);
 b.flags=0x10;b.stride=128;
 assert(nvn_sdk_texture_storage_size(0,&b)==2048);
 assert(nvn_sdk_texture_storage_alignment(0,&b)==32);
 b.flags=0;b.target=10;b.width=123;
 assert(nvn_sdk_texture_storage_size(0,&b)==492);
 b.target=4;b.width=16;b.depth=3;
 assert(nvn_sdk_texture_storage_size(0,&b)==3072);
 b.target=1;b.depth=1;b.format=0x41;b.width=4;b.height=4;
 assert(nvn_sdk_texture_storage_size(0,&b)==512);
 assert(!nvn_sdk_format_known(0xffff));
 puts("Scarlet texture storage: tiled, linear, buffer, array and BC1 passed");
 return 0;
}
