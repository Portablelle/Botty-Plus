// Launcher registration follows ps5-payload-dev/websrv (GPL-3.0-or-later),
// commit baabe27e5449baeb059b850d0393c31fdee219b7, src/ps5/sys.c.
#include "core.hpp"
#include <ps5/kernel.h>
#include <iostream>
#include <fstream>
#include <stdexcept>
extern "C" {
int sceUserServiceInitialize(void*);
int sceAppInstUtilInitialize();
int sceAppInstUtilAppInstallAll(void*);
}
void installHomeIcon() {
  using namespace botty;
  constexpr const char* title="BTTY00001";
  const fs::path app="/user/app/BTTY00001";
  sceUserServiceInitialize(nullptr);
  kernel_set_ucred_authid(-1,0x4801000000000013L);
  const int initialized=sceAppInstUtilInitialize();
  if(initialized)throw std::runtime_error("Cannot initialize PS5 application registration");
  const fs::path param=app/"sce_sys/param.json";
  const json metadata={{"titleId",title},{"deeplinkUri","http://127.0.0.1:8088/"},
    {"localizedParameters",{{"defaultLanguage","en-US"},{"en-US",{{"titleName","Botty Downloads"}}}}}};
  if(fs::exists(param)) {
    auto existing=json::parse(readText(param));
    if(existing!=metadata)throw std::runtime_error("Botty title ID is occupied by a different application");
  } else {
    if(fs::exists(app))throw std::runtime_error("Botty application directory exists but is incomplete; inspect it before retrying");
    fs::create_directories(app/"sce_sys");
    // Console shell needs readable metadata/icon independent of its effective UID.
    fs::permissions(app,fs::perms::owner_all|fs::perms::group_read|fs::perms::group_exec|fs::perms::others_read|fs::perms::others_exec);
    fs::permissions(app/"sce_sys",fs::perms::owner_all|fs::perms::group_read|fs::perms::group_exec|fs::perms::others_read|fs::perms::others_exec);
    fs::copy_file("/data/botty/manager/0.1.5/icon0.png",app/"sce_sys/icon0.png");
    writeJson(param,metadata);
    for(const auto& file:{param,app/"sce_sys/icon0.png"})fs::permissions(file,fs::perms::owner_read|fs::perms::owner_write|fs::perms::group_read|fs::perms::others_read);
  }
  uint32_t handle=0;
  int (*install)(const char*,const char*,void*)=nullptr;
  if(!kernel_dynlib_handle(-1,"libSceAppInstUtil.sprx",&handle))
    install=reinterpret_cast<decltype(install)>(kernel_dynlib_resolve(-1,handle,"Wudg3Xe3heE"));
  const int result=install?install(title,"/user/app/",nullptr):sceAppInstUtilAppInstallAll(nullptr);
  if(result)throw std::runtime_error("PS5 rejected Botty icon registration: "+std::to_string(result));
}
