// SPDX-License-Identifier: GPL-3.0-or-later
#include "switch-preparation.hpp"
#include "progress.hpp"
#include <iostream>
int main(int argc,char** argv){
 using namespace botty;
#ifdef __PS5__
 ::setuid(0);::setgid(0);
#endif
 ::umask(0077);
 try{
  fs::path request=argc==2?argv[1]:"/data/botty/switch-pipeline-test/request.json";
  const auto input=json::parse(readText(request,1024*1024));
  const fs::path keyFile=input.value("keys",std::string{});
  if(input.value("operation",std::string{})=="inspect"){
   SwitchKeys keys(keyFile);json results=json::array();
   for(const auto& file:input.at("files")){try{auto m=inspectSwitchMetadata(file.get<std::string>(),keys);m["file"]=fs::path(file.get<std::string>()).filename().string();results.push_back(m);}catch(const std::exception& e){results.push_back({{"file",fs::path(file.get<std::string>()).filename().string()},{"error",e.what()}});}}
   writeJson(input.at("result").get<std::string>(),results);return 0;
  }
  ProgressSchedule schedule;
  const auto report=[&](const Progress& p){if(schedule.update(std::chrono::steady_clock::now(),p.phase).publish)std::cout<<json({{"phase",p.phase},{"file",p.file},{"bytes",p.bytes},{"total",p.total}}).dump()<<'\n'<<std::flush;};
  auto result=prepareSwitchFiles(input.at("root").get<std::string>(),input.at("files"),input.at("output").get<std::string>(),keyFile,input.value("name",std::string("Switch download")),report);
  result["transaction"]=input.at("transaction");writeJson(fs::path(input.at("output").get<std::string>())/"prepared.json",result);
  if(input.contains("publishTo"))publishSwitchBatch(input.at("output").get<std::string>(),input.at("publishTo").get<std::string>(),input.at("transaction").get<std::string>());
  std::cout<<json({{"status","ready"}}).dump()<<'\n'<<std::flush;return 0;
 }catch(const std::exception& error){
  const json result={{"status","failed"},{"error",error.what()}};
  std::cerr<<result.dump()<<'\n';
#ifdef __PS5__
  try{writeJson("/data/botty/switch-pipeline-test/error.json",result);}catch(...){}
#endif
  return 1;
 }
}
