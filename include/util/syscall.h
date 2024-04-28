#ifndef WAYWALL_UTIL_SYSCALL_H
#define WAYWALL_UTIL_SYSCALL_H

#include <signal.h>
#include <sys/types.h>

int memfd_create(const char *name, unsigned int flags);
int pidfd_open(pid_t pid, unsigned int flags);
int pidfd_send_signal(int pidfd, int sig, siginfo_t *info, unsigned int flags);

#endif
