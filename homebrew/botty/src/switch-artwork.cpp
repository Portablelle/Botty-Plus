// SPDX-License-Identifier: GPL-3.0-or-later
#include "switch-library.hpp"
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 1024
#define STB_IMAGE_IMPLEMENTATION
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-parameter"
#endif
#include "stb_image.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
namespace botty {
std::string switchLibraryPixels(const std::vector<unsigned char>& encoded) {
  if(encoded.empty()||encoded.size()>2*1024*1024)return {};
  const bool jpeg=encoded.size()>=3&&encoded[0]==0xff&&encoded[1]==0xd8&&encoded[2]==0xff;
  if(!jpeg){
    // Eden writes uncompressed true-colour TGA. stb's raw TGA path does not
    // reject short row reads, so require the complete pixel extent up front.
    if(encoded.size()<18||encoded[1]!=0||encoded[2]!=2||(encoded[16]!=24&&encoded[16]!=32))return {};
    const uint64_t w=switchLE(encoded.data()+12,2),h=switchLE(encoded.data()+14,2);
    const uint64_t at=18+encoded[0],bytes=w*h*(encoded[16]/8);
    if(!w||!h||w>1024||h>1024||at>encoded.size()||bytes>encoded.size()-at)return {};
  }
  int width=0,height=0,channels=0;
  if(!stbi_info_from_memory(encoded.data(),int(encoded.size()),&width,&height,&channels)||width<=0||height<=0||width>1024||height>1024)return {};
  auto* pixels=stbi_load_from_memory(encoded.data(),int(encoded.size()),&width,&height,&channels,3);
  if(!pixels)return {};
  struct Free{unsigned char* pixels;~Free(){stbi_image_free(pixels);}} release{pixels};
  std::string out(160*240*3,'\0');
  const double scale=std::min(160.0/width,240.0/height);
  const int w=std::max(1,int(width*scale)),h=std::max(1,int(height*scale));
  for(int y=0;y<h;++y)for(int x=0;x<w;++x)for(int c=0;c<3;++c)
    out[((y+(240-h)/2)*160+x+(160-w)/2)*3+c]=char(pixels[((y*height/h)*width+x*width/w)*3+c]);
  return out;
}
}
