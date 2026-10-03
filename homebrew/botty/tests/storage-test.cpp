#include "storage.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
using namespace botty;
template<class F> void rejects(F f){bool failed=false;try{f();}catch(...){failed=true;}assert(failed);}
int main(){
 auto root=fs::temp_directory_path()/("botty-storage-"+randomId());fs::create_directories(root/"internal");fs::create_directories(root/"usb");
 Paths paths(root/"internal",root/"internal/library");Storage storage;storage.init(paths,{root/"usb"});auto list=storage.list();assert(list.size()==2);auto id=list[1].at("id").get<std::string>();const auto external=storage.get(id);assert(external.jobs==paths.jobs&&external.library==root/"usb/homebrew");
 fs::create_directories(root/"source/nested");std::ofstream(root/"source/nested/data")<<"verified data";fs::create_directory(root/"target");
 copyChecked(root/"source",root/"target/copy");assert(readText(root/"target/copy/nested/data")=="verified data");assert(fs::exists(root/"source/nested/data"));
 rejects([&]{copyChecked(root/"source",root/"target/copy");});
 std::ofstream(root/"too-large")<<"";fs::resize_file(root/"too-large",freeBytes(root)+1073741824ULL);rejects([&]{copyChecked(root/"too-large",root/"target/too-large");});assert(!fs::exists(root/"target/too-large"));fs::remove(root/"too-large");
 fs::create_symlink(root/"source/nested/data",root/"source/link");rejects([&]{copyChecked(root/"source",root/"target/unsafe");});fs::remove(root/"source/link");
 int checks=0;rejects([&]{copyChecked(root/"source",root/"target/interrupted",[&]{if(++checks==3)throw std::runtime_error("Disconnected");});});assert(fs::exists(root/"source/nested/data")&&!fs::exists(root/"target/interrupted"));
 fs::rename(root/"usb",root/"removed");rejects([&]{storage.get(id);});assert(!fs::exists(root/"usb"));fs::create_directory(root/"usb");rejects([&]{storage.get(id);});assert(!fs::exists(root/"usb/botty"));
 auto replacement=storage.list();assert(replacement.size()==3);auto other=replacement[1].at("id").get<std::string>();assert(other!=id);
 fs::create_directory_symlink(root/"internal",root/"usb/botty");rejects([&]{storage.get(other);});
 removeTransferred(root/"source");assert(!fs::exists(root/"source")&&fs::exists(root/"target/copy/nested/data"));fs::remove_all(root);
 std::cout<<"Storage identities, disconnect, replacement, confinement, verified copy and source preservation passed\n";
}
