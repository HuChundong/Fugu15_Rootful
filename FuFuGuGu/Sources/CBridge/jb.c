#include <stdarg.h>
#include <string.h>
#include <mach/mach.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <spawn.h>
#include <sys/mount.h>
#include <xpc/xpc.h>
#include <bootstrap.h>

#include "init.h"

#define INJECT_KEY "DYLD_INSERT_LIBRARIES"
#define INJECT_VALUE "/usr/lib/jbinjector.dylib"
#define INJECT_KEY2 "DYLD_AMFI_FAKE"
#define INJECT_VALUE2 "0xff"

#ifndef DYLD_INTERPOSE
#define DYLD_INTERPOSE(_replacment,_replacee) \
__attribute__((used)) static struct{ const void* replacment; const void* replacee; } _interpose_##_replacee \
__attribute__ ((section ("__DATA,__interpose"))) = { (const void*)(unsigned long)&_replacment, (const void*)(unsigned long)&_replacee };
#endif

int xpc_receive_mach_msg(void *a1, void *a2, void *a3, void *a4, xpc_object_t *a5, void *a6, void *a7, void *a8);
int xpc_pipe_routine_reply(xpc_object_t reply);
xpc_object_t xpc_mach_send_create_with_disposition(mach_port_t port, int disposition);
void xpc_dictionary_get_audit_token(xpc_object_t, audit_token_t *);

#pragma clang diagnostic ignored "-Wavailability"
pid_t audit_token_to_pid(audit_token_t atoken) API_AVAILABLE(ios(15));
int audit_token_to_pidversion(audit_token_t atoken) API_AVAILABLE(ios(15));

void *my_malloc(size_t sz) {
    int fd_console = open("/dev/console",O_RDWR,0);
    void *res = malloc(sz);
    dprintf(fd_console, "malloc %zu -> %p\n", sz, res);
    usleep(10000);
    close(fd_console);
    
    return res;
}

//#define free(ptr) {int fd_console = open("/dev/console",O_RDWR,0); dprintf(fd_console, "Freeing %p\n", ptr); usleep(10000); free(ptr); close(fd_console);}
//#define malloc my_malloc

#define safeClose(fd) do{if ((fd) != -1){close(fd); fd = -1;}}while(0)
#define safeFree(buf) do{if ((buf)){free(buf); buf = NULL;}}while(0)
#define assure(cond) do {if (!(cond)){err = __LINE__; goto error;}}while(0)

extern void swift_init(int console_fd, mach_port_t servicePort, mach_port_t *XPCServicePort);
extern int isTokenBlacklisted(audit_token_t au);
extern int sysctlbyname_get_data_np(const char *name, void **buf, size_t *len);

uint64_t gUserReturnDidHappen;

mach_port_t bp = 0;
mach_port_t servicePort = 0;
bool didRegisterJBDService = false;

int sandbox_check_by_audit_token(audit_token_t au, const char *operation, int sandbox_filter_type, ...);
int my_sandbox_check_by_audit_token(audit_token_t au, const char *operation, int sandbox_filter_type, ...) {
    va_list a;
    va_start(a, sandbox_filter_type);
    const char *name = va_arg(a, const char *);
    const void *arg2 = va_arg(a, void *);
    const void *arg3 = va_arg(a, void *);
    const void *arg4 = va_arg(a, void *);
    const void *arg5 = va_arg(a, void *);
    const void *arg6 = va_arg(a, void *);
    const void *arg7 = va_arg(a, void *);
    const void *arg8 = va_arg(a, void *);
    const void *arg9 = va_arg(a, void *);
    const void *arg10 = va_arg(a, void *);
    va_end(a);
    if (name && operation) {
        if (strcmp(operation, "mach-lookup") == 0) {
            if (strncmp((char *)name, "jb-global-", sizeof("jb-global-")-1) == 0) {
                if (!isTokenBlacklisted(au)) {
                  return 0;
                }
            }
        }
    }
    return sandbox_check_by_audit_token(au, operation, sandbox_filter_type, name, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10);
}
DYLD_INTERPOSE(my_sandbox_check_by_audit_token, sandbox_check_by_audit_token);

extern char **environ;

int my_kill(pid_t pid, int sig) {
    if (pid == -1 && sig == SIGKILL){
        int fd_console = open("/dev/console", O_RDWR, 0);
        dprintf(fd_console, "Launchd is about to restart userspace (hopefully!), doing execve...\n");
        if (bp != 0) {
            kern_return_t kr = task_set_bootstrap_port(mach_task_self_, bp);
            if (kr != KERN_SUCCESS) {
                dprintf(fd_console, "Stashed jailbreakd port!\n");
            } else {
                dprintf(fd_console, "No task_set_bootstrap_port for you...\n");
            }
        } else {
            dprintf(fd_console, "No bootstrap port - let's hope it's set it already...\n");
        }
        
        setenv("XPC_USERSPACE_REBOOTED", "1", 1);
        setenv("DYLD_INSERT_LIBRARIES", "/usr/lib/libFuFuGuGu.dylib", 1);
        setenv("DYLD_AMFI_FAKE", "0xFF", 1);
        
        close(fd_console);
        
        uint32_t val = 1;
        sysctlbyname("vm.shared_region_pivot", 0LL, 0LL, &val, 4uLL);
        
        char * const argv[] = { "/sbin/launchd", NULL };
        execve("/sbin/launchd", argv, environ);
        return 0;
    }
    return kill(pid, sig);
}
DYLD_INTERPOSE(my_kill, kill);

xpc_object_t my_xpc_dictionary_get_value(xpc_object_t dict, const char *key) {
    xpc_object_t retval = xpc_dictionary_get_value(dict, key);
    if (strcmp(key, "LaunchDaemons") == 0) {
        /*xpc_object_t programArguments = xpc_array_create(NULL, 0);
        xpc_array_append_value(programArguments, xpc_string_create("/sbin/babyd"));
        
        xpc_object_t submitJob = xpc_dictionary_create(NULL, NULL, 0);
        xpc_dictionary_set_bool(submitJob, "KeepAlive", false);
        xpc_dictionary_set_bool(submitJob, "RunAtLoad", true);
        xpc_dictionary_set_string(submitJob, "UserName", "root");
        xpc_dictionary_set_string(submitJob, "Program", "/sbin/babyd");
        xpc_dictionary_set_string(submitJob, "Label", "de.pinauten.babyd");
        xpc_dictionary_set_value(submitJob, "ProgramArguments", programArguments);
        
        xpc_dictionary_set_value(retval, "/System/Library/LaunchDaemons/de.pinauten.babyd.plist", submitJob);*/
    }
    
    return retval;
}
DYLD_INTERPOSE(my_xpc_dictionary_get_value, xpc_dictionary_get_value);

void injectDylibToEnvVars(char *const envp[], char ***outEnvp, char **freeme) {
    bool key1Seen = false;
    bool key2Seen = false;
    
    size_t envCount = 0;
    while (envp[envCount] != NULL) {
        envCount++;
    }
    
    char **newEnvp = malloc((envCount + 3) * sizeof(char*));
    memset(newEnvp, 0, (envCount + 3) * sizeof(char*));
    
    for (size_t i = 0; i < envCount; i++) {
        char *env = envp[i];
        
        if (!key1Seen && !strncmp(envp[i], INJECT_KEY "=", sizeof(INJECT_KEY))) {
            if (strncmp(envp[i], INJECT_KEY "=" INJECT_VALUE ":", sizeof(INJECT_KEY "=" INJECT_VALUE)) && strcmp(envp[i], INJECT_KEY "=" INJECT_VALUE)) {
                char *var = malloc(strlen(envp[i]) + sizeof(INJECT_VALUE ":"));
                sprintf(var, "%s=%s:%s", INJECT_KEY, INJECT_VALUE, envp[i] + sizeof(INJECT_KEY));
                freeme[0] = var;
                newEnvp[i] = var;
                key1Seen = true;
                continue;
            }
        } else if (!key2Seen && !strncmp(envp[i], INJECT_KEY2 "=", sizeof(INJECT_KEY2))) {
            if (strncmp(envp[i], INJECT_KEY2 "=" INJECT_VALUE2 ":", sizeof(INJECT_KEY2 "=" INJECT_VALUE2)) && strcmp(envp[i], INJECT_KEY2 "=" INJECT_VALUE2)) {
                char *var = malloc(strlen(envp[i]) + sizeof(INJECT_VALUE2 ":"));
                sprintf(var, "%s=%s:%s", INJECT_KEY2, INJECT_VALUE2, envp[i] + sizeof(INJECT_KEY2));
                freeme[1] = var;
                newEnvp[i] = var;
                key2Seen = true;
                continue;
            }
        }
        
        newEnvp[i] = envp[i];
    }
    
    if (!key1Seen) {
        newEnvp[envCount] = INJECT_KEY "=" INJECT_VALUE;
        envCount++;
    }
    
    if (!key2Seen) {
        newEnvp[envCount] = INJECT_KEY2 "=" INJECT_VALUE2;
    }
    
    *outEnvp = newEnvp;
}

int my_posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *file_actions, const posix_spawnattr_t *attrp, char *const argv[], char *const envp[]){
    int fd_console = open("/dev/console",O_RDWR,0);
    dprintf(fd_console, "spawning %s\n", path);
    close(fd_console);
    
    int ret = 0;
    char **out = NULL;
    char *freeme[2] = { NULL, NULL };
    //trustCDHashesForBinary(path);
    injectDylibToEnvVars(envp, &out, freeme);
    if (out)
        envp = out;
    ret = posix_spawn(pid, path, file_actions, attrp, argv, envp);
error:
    safeFree(out);
    safeFree(freeme[0]);
    safeFree(freeme[1]);
    return ret;
}
DYLD_INTERPOSE(my_posix_spawn, posix_spawn);

int my_posix_spawnp(pid_t *pid, const char *path, const posix_spawn_file_actions_t *file_actions, const posix_spawnattr_t *attrp, char *const argv[], char *const envp[]){
    int fd_console = open("/dev/console",O_RDWR,0);
    dprintf(fd_console, "spawning %s\n", path);
    close(fd_console);
    
    int ret = 0;
    char **out = NULL;
    char *freeme[2] = { NULL, NULL };
    //trustCDHashesForBinary(path);
    injectDylibToEnvVars(envp, &out, freeme);
    if (out)
        envp = out;
    ret = posix_spawnp(pid, path, file_actions, attrp, argv, envp);
error:
    safeFree(out);
    safeFree(freeme[0]);
    safeFree(freeme[1]);
    return ret;
}
DYLD_INTERPOSE(my_posix_spawnp, posix_spawnp);

int sendPort(mach_port_t to, mach_port_t port) {
    struct {
        mach_msg_header_t          header;
        mach_msg_body_t            body;
        mach_msg_port_descriptor_t task_port;
    } msg;
    
    msg.header.msgh_remote_port = to;
    msg.header.msgh_local_port = MACH_PORT_NULL;
    msg.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) | MACH_MSGH_BITS_COMPLEX;
    msg.header.msgh_size = sizeof(msg);
    
    msg.body.msgh_descriptor_count = 1;
    msg.task_port.name = port;
    msg.task_port.disposition = MACH_MSG_TYPE_COPY_SEND;
    msg.task_port.type = MACH_MSG_PORT_DESCRIPTOR;
    
    kern_return_t kr = mach_msg_send(&msg.header);
    if (kr != KERN_SUCCESS) {
        return 1;
    }
    
    return 0;
}

mach_port_t recvPort(mach_port_t from) {
    struct {
        mach_msg_header_t          header;
        mach_msg_body_t            body;
        mach_msg_port_descriptor_t task_port;
        mach_msg_trailer_t         trailer;
        uint64_t                   pad[20];
    } msg;
    
    kern_return_t kr = mach_msg(&msg.header, MACH_RCV_MSG, 0, sizeof(msg), from, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (kr != KERN_SUCCESS) {
        return MACH_PORT_NULL;
    }
    
    return msg.task_port.name;
}

int my_xpc_receive_mach_msg(void *a1, void *a2, void *a3, void *a4, xpc_object_t *object_out, void *a6, void *a7, void *a8) {
    int err = xpc_receive_mach_msg(a1, a2, a3, a4, object_out, a6, a7, a8);
    if (err == 0 && object_out && *object_out && servicePort) {
        if (xpc_get_type(*object_out) == XPC_TYPE_DICTIONARY) {
            xpc_object_t dict = *object_out;
            uint64_t type = xpc_dictionary_get_uint64(dict, "type");
            if (type == 7) {
                const char *name = xpc_dictionary_get_string(dict, "name");
                if (name && strcmp(name, "jb-global-jbd") == 0) {
                    xpc_object_t port = xpc_mach_send_create_with_disposition(servicePort, MACH_MSG_TYPE_MAKE_SEND);
                    if (port) {
                        xpc_object_t *reply = xpc_dictionary_create_reply(dict);
                        if (reply) {
                            xpc_dictionary_set_value(reply, "port", port);
                            xpc_release(port);
                            
                            audit_token_t token;
                            xpc_dictionary_get_audit_token(dict, &token);
                            
                            pid_t pid = audit_token_to_pid(token);
                            int execcnt = audit_token_to_pidversion(token);
                            
                            xpc_dictionary_set_uint64(reply, "req_pid", (uint64_t) pid);
                            xpc_dictionary_set_uint64(reply, "rec_execcnt", (uint64_t) execcnt);
                            
                            xpc_pipe_routine_reply(reply);
                            xpc_release(reply);
                            xpc_release(dict);
                            
                            return 22;
                        }
                        
                        xpc_release(port);
                    }
                }
            }
        }
        
        char *desc = xpc_copy_description(*object_out);
        int fd_console = open("/dev/console", O_RDWR, 0);
        dprintf(fd_console, "XPC Request: %s\n", desc);
        close(fd_console);
        free(desc);
    }
    
    return err;
}
DYLD_INTERPOSE(my_xpc_receive_mach_msg, xpc_receive_mach_msg);

__attribute__((constructor))
static void customConstructor(int argc, const char **argv){
    int fd_console = open("/dev/console",O_RDWR,0);
    dprintf(fd_console,"================ Hello from Stage 2 dylib ================ \n");
    
    dprintf(fd_console,"I can haz bootstrap port?\n");
    kern_return_t kr = task_get_bootstrap_port(mach_task_self_, &bp);
    if (kr == KERN_SUCCESS) {
        if (!MACH_PORT_VALID(bp)) {
            dprintf(fd_console,"No bootstrap port, no KRW, nothing I can do, goodbye!\n");
            //return;
            bp = 0;
        } else {
            dprintf(fd_console,"Got bootstrap port!\n");
        }
    } else {
        dprintf(fd_console,"No task_get_bootstrap_port???\n");
        dprintf(fd_console,"No bootstrap port, no KRW, nothing I can do, goodbye!\n");
        return;
    }
    
    swift_init(fd_console, bp, &servicePort);

    dprintf(fd_console,"========= Goodbye from Stage 2 dylib constructor ========= \n");
    close(fd_console);
}

void dmb_sy(void) {
    asm volatile("dmb sy");
}