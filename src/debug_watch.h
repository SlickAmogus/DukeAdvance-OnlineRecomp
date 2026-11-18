#pragma once
#include <cstddef>

// Game-owned debug TCP commands (RunOptions::tcp_command). See debug_watch.cpp.
int duke_debug_tcp_command(const char* request,
                           void (*write)(void*, const char*, std::size_t),
                           void* ctx);
