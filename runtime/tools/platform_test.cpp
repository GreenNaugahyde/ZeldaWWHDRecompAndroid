// Portable host filesystem/thread helpers; never touches user game/save data.
#include "platform/host.h"
#include "platform/filesystem.h"
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
int main(){
 auto root=std::filesystem::temp_directory_path()/("wwhd-platform-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 std::filesystem::create_directories(root/"sub");
 auto path=(root/"sub"/"sparse.bin").string();FILE* f=fopen(path.c_str(),"w+b");assert(f);
 uint64_t offset=(1ull<<32)+123;assert(host::file_seek(f,offset,SEEK_SET)==0);assert(fputc(42,f)==42);assert(host::file_tell(f)==(int64_t)offset+1);assert(host::file_seek(f,offset,SEEK_SET)==0);assert(fgetc(f)==42);fclose(f);
 DIR* d=opendir((root/"sub").string().c_str());assert(d);bool found=false;while(auto* e=readdir(d))found|=strcmp(e->d_name,"sparse.bin")==0;assert(found);assert(!readdir(d));closedir(d);
 auto to=(root/"replaced.bin").string();f=fopen(to.c_str(),"wb");assert(f);fclose(f);assert(host::replace_file(path,to));assert(std::filesystem::file_size(to)==offset+1);
 std::thread thread([]{host::set_thread_name("platform test");char name[64];host::get_thread_name(name,sizeof name);assert(strcmp(name,"platform test")==0);});thread.join();
 assert(host::page_size()>=4096);assert(host::executable_base()!=0);assert(!host::config_dir().empty());
 std::filesystem::remove_all(root);puts("platform_test: 64-bit files, directories, atomic replacement, thread names passed");
}
