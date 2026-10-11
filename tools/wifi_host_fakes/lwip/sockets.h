#pragma once
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
// lwIP names the keepalive idle option TCP_KEEPIDLE, as Linux does. macOS
// calls the same option TCP_KEEPALIVE.
#if defined(__APPLE__) && !defined(TCP_KEEPIDLE)
#define TCP_KEEPIDLE TCP_KEEPALIVE
#endif
