#pragma once

/**
 * Single include point for libssh. The library headers are C headers written for warning
 * level 3; the application builds with /W4 (MSVC) and -Wall -Wextra -Wpedantic (GCC/Clang)
 * plus warnings-as-errors in CI, so they are pulled in with the warning level relaxed.
 * Only SshConnection.cpp and the test server include this; no libssh type leaks into a
 * public header.
 */
#if defined(_MSC_VER)
#pragma warning(push, 3)
#pragma warning(disable : 4200 4201 4244 4267 4996)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

#include <libssh/libssh.h>
#include <libssh/callbacks.h>
#include <libssh/sftp.h>
#include <libssh/server.h>
#include <libssh/sftpserver.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
