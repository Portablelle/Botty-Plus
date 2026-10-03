#include <assert.h>
#include <string.h>
#include "cr_remote_sources.h"
const char *path_basename_ptr(const char *p){const char *s=strrchr(p,'/');return s?s+1:p;}
int main(void){char v[64];extract_version_from_filename("cheats/mc4/PPSA31246_01.200.000_ef4d3664.mc4",v,sizeof v);assert(!strcmp(v,"01.200.000"));extract_version_from_filename("PPSA31246_02.013.000_d471e4d7.mc4",v,sizeof v);assert(!strcmp(v,"02.013.000"));extract_version_from_filename("CUSA00001_01.01.json",v,sizeof v);assert(!strcmp(v,"01.01"));extract_version_from_filename("PPSA31246_invalid.mc4",v,sizeof v);assert(!v[0]);return 0;}
