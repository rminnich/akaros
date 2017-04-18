#pragma once

#include <parlib/common.h>
#include <parlib/parlib.h>
#include <parlib/vcore.h>
#include <unistd.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>

__BEGIN_DECLS

#define debug_printf(...) {                                                    \
	char buf[128];                                                             \
	int ret = snprintf(buf, sizeof(buf), __VA_ARGS__);                         \
	write(2, buf, ret);                                                        \
}

#define I_AM_HERE debug_printf("PID %d, vcore %d is in %s() at %s:%d\n",       \
                               getpid(), vcore_id(), __FUNCTION__, __FILE__,   \
                               __LINE__);

void trace_printf(const char *fmt, ...);

/* For a poor-mans function tracer (can add these with spatch) */
void __print_func_entry(const char *func, const char *file);
void __print_func_exit(const char *func, const char *file);
#define print_func_entry() __print_func_entry(__FUNCTION__, __FILE__)
#define print_func_exit() __print_func_exit(__FUNCTION__, __FILE__)

/* user/parlib/hexdump.c */
void hexdump(FILE *f, void *v, int length);

void fprintf_hw_tf(FILE *f, struct hw_trapframe *hw_tf);
void fprintf_sw_tf(FILE *f, struct sw_trapframe *sw_tf);
void fprintf_vm_tf(FILE *f, struct vm_trapframe *vm_tf);
void print_user_context(struct user_context *ctx);

__END_DECLS
