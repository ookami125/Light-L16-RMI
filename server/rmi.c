#include <poll.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <grp.h>
#include <arpa/inet.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <linux/input.h>
#include <sys/stat.h>
#include <limits.h>
#include <dirent.h>

#include "rmi_version.h"
#include "rmi_protocol.h"

#define DEFAULT_IP            INADDR_LOOPBACK
#define DEFAULT_PORT          1234
#define RMI_HEARTBEAT_MS      5000
#define RMI_CONFIG_PATH       "/data/local/tmp/rmi.config"
#define RMI_DEFAULT_USER      "l16"
#define RMI_DEFAULT_PASS      "l16"
#define RMI_LOG_PATH          "/data/local/tmp/rmi.log"
#define AID_SHELL             2000
#define RMI_LIST_MAX_BYTES    (1024u * 1024u)

#define CHECKSYSCALL(r, name) \
    if((r)==-1){fprintf(stderr,"Syscall error: %s at line %d " \
        "with code %d.\n",name,__LINE__,errno);exit(EXIT_FAILURE);}

static char **rmi_argv;

enum rmi_client_result {
    RMI_CONTINUE = 0,
    RMI_SHUTDOWN = 1,
    RMI_RESTART = 2,
};

static void
redirect_rmi_logs(void)
{
    int fd;

    fd = open(RMI_LOG_PATH, O_CREAT | O_WRONLY | O_APPEND, 0666);
    if (fd == -1)
    {
        return;
    }
    fchmod(fd, 0666);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    if (fd > STDERR_FILENO)
    {
        close(fd);
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
}

static int
get_self_path(char *buf, size_t len)
{
    ssize_t read_len;
    const char suffix[] = " (deleted)";
    size_t suffix_len;

    if (buf == NULL || len == 0)
    {
        return -1;
    }
    read_len = readlink("/proc/self/exe", buf, len - 1);
    if (read_len >= 0)
    {
        buf[read_len] = '\0';
        suffix_len = strlen(suffix);
        if ((size_t)read_len > suffix_len)
        {
            size_t pos = (size_t)read_len - suffix_len;
            if (strcmp(buf + pos, suffix) == 0)
            {
                buf[pos] = '\0';
            }
        }
        return 0;
    }
    if (rmi_argv != NULL && rmi_argv[0] != NULL)
    {
        snprintf(buf, len, "%s", rmi_argv[0]);
        suffix_len = strlen(suffix);
        if (strlen(buf) > suffix_len)
        {
            size_t pos = strlen(buf) - suffix_len;
            if (strcmp(buf + pos, suffix) == 0)
            {
                buf[pos] = '\0';
            }
        }
        return 0;
    }
    return -1;
}

static bool
is_self_binary_path(const char *path)
{
    char self_path[PATH_MAX];

    if (path == NULL)
    {
        return false;
    }
    if (get_self_path(self_path, sizeof(self_path)) == 0)
    {
        if (strcmp(path, self_path) == 0)
        {
            return true;
        }
    }
    return strcmp(path, "/data/local/tmp/rmi") == 0;
}

static void
drop_to_shell_user(void)
{
    if (getuid() != 0 && geteuid() != 0)
    {
        return;
    }
    fprintf(stderr, "RMI press_input: dropping to shell user\n");
    if (setgroups(0, NULL) == -1)
    {
        fprintf(stderr, "RMI press_input: setgroups failed: %d\n", errno);
    }
    if (setgid(AID_SHELL) == -1)
    {
        fprintf(stderr, "RMI press_input: setgid failed: %d\n", errno);
    }
    if (setuid(AID_SHELL) == -1)
    {
        fprintf(stderr, "RMI press_input: setuid failed: %d\n", errno);
    }
}

static void
set_shell_env(void)
{
    setenv("PATH", "/sbin:/vendor/bin:/system/sbin:/system/bin:/system/xbin", 1);
    setenv("ANDROID_ROOT", "/system", 1);
    setenv("ANDROID_DATA", "/data", 1);
    setenv("ANDROID_RUNTIME_ROOT", "/system", 1);
    setenv("ANDROID_ASSETS", "/system/app", 1);
    setenv("ANDROID_BOOTLOGO", "1", 1);
    setenv("ANDROID_STORAGE", "/storage", 1);
    setenv("EXTERNAL_STORAGE", "/sdcard", 1);
    setenv("ASEC_MOUNTPOINT", "/mnt/asec", 1);
    setenv("TMPDIR", "/data/local/tmp", 1);
    setenv("HOME", "/data", 1);
    setenv("USER", "shell", 1);
    setenv("SHELL", "/system/bin/sh", 1);
    setenv("MKSH", "/system/bin/sh", 1);
    setenv("TERM", "xterm", 1);
    setenv("BOOTCLASSPATH",
           "/system/framework/core-libart.jar:/system/framework/conscrypt.jar"
           ":/system/framework/okhttp.jar:/system/framework/core-junit.jar"
           ":/system/framework/bouncycastle.jar:/system/framework/ext.jar"
           ":/system/framework/framework.jar:/system/framework/telephony-common.jar"
           ":/system/framework/voip-common.jar:/system/framework/ims-common.jar"
           ":/system/framework/apache-xml.jar:/system/framework/org.apache.http.legacy.boot.jar"
           ":/system/framework/tcmiface.jar:/system/framework/WfdCommon.jar"
           ":/system/framework/com.qti.dpmframework.jar:/system/framework/dpmapi.jar"
           ":/system/framework/com.qti.location.sdk.jar:/system/framework/oem-services.jar"
           ":/system/framework/qcmediaplayer.jar",
           1);
    setenv("SYSTEMSERVERCLASSPATH",
           "/system/framework/services.jar:/system/framework/ethernet-service.jar"
           ":/system/framework/wifi-service.jar",
           1);
    setenv("LD_LIBRARY_PATH",
           "/system/lib64:/vendor/lib64:/system/lib:/vendor/lib",
           1);
}

static void
log_identity(const char *tag)
{
    char ctx[128];
    int fd;
    ssize_t n;

    ctx[0] = '\0';
    fd = open("/proc/self/attr/current", O_RDONLY | O_CLOEXEC);
    if (fd >= 0)
    {
        n = read(fd, ctx, sizeof(ctx) - 1);
        close(fd);
        if (n > 0)
        {
            ctx[n] = '\0';
            if (ctx[n - 1] == '\n')
            {
                ctx[n - 1] = '\0';
            }
        }
        else
        {
            ctx[0] = '\0';
        }
    }

    fprintf(stderr, "RMI press_input: %s uid=%d gid=%d context=%s\n",
            tag, (int)getuid(), (int)getgid(), ctx[0] ? ctx : "unknown");
}

static int
setup_socket(uint16_t port)
{
    struct sockaddr_in addr;
    int enable, s;

    s = socket(AF_INET, SOCK_STREAM, 0);
    CHECKSYSCALL(s, "socket");

    enable = 1;
    CHECKSYSCALL(setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
                            &enable, sizeof(enable)), "setsockopt");

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = port;

    CHECKSYSCALL(bind(s, (struct sockaddr *) &addr, sizeof(addr)), "bind");

    CHECKSYSCALL(listen(s, 1), "listen");

    return s;
}

static int
writeall(int fd, const void *buf, size_t count)
{
    const char *p;
    ssize_t i;

    p = buf;
    do
    {
        i = write(fd, p, count);
        if (i == 0)
        {
            return -1;
        }
        else if (i == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return -1;
        }
        count -= i;
        p += i;
    }
    while (count > 0);

    return 0;
}

static int
read_exact(int fd, void *buf, size_t count)
{
    size_t done;

    done = 0;
    while (done < count)
    {
        ssize_t n;

        n = read(fd, (char *)buf + done, count - done);
        if (n == 0)
        {
            return 0;
        }
        if (n == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return -1;
        }
        done += (size_t)n;
    }

    return 1;
}

static int
read_frame_size(int fd, uint32_t *out_len)
{
    uint8_t header[RMI_FRAME_HEADER_SIZE];
    int rc;

    rc = read_exact(fd, header, sizeof(header));
    if (rc <= 0)
    {
        return -1;
    }
    *out_len = rmi_read_be32(header);
    return 0;
}

static int
drain_bytes(int fd, uint32_t len)
{
    char buf[4096];
    uint32_t remaining;

    remaining = len;
    while (remaining > 0)
    {
        size_t chunk;
        int rc;

        chunk = remaining > sizeof(buf) ? sizeof(buf) : remaining;
        rc = read_exact(fd, buf, chunk);
        if (rc <= 0)
        {
            return -1;
        }
        remaining -= (uint32_t)chunk;
    }
    return 0;
}

static int
recv_frame_to_file(int fd, const char *path, uint32_t expected_len)
{
    uint32_t len;
    uint32_t remaining;
    int out_fd;
    const char *write_path;
    char tmp_path[PATH_MAX];
    bool use_tmp;

    if (read_frame_size(fd, &len) == -1)
    {
        return -1;
    }
    if (len != expected_len)
    {
        drain_bytes(fd, len);
        return -1;
    }

    write_path = path;
    use_tmp = false;
    if (is_self_binary_path(path))
    {
        if (snprintf(tmp_path, sizeof(tmp_path), "%s.new", path) >= (int)sizeof(tmp_path))
        {
            drain_bytes(fd, len);
            return -1;
        }
        write_path = tmp_path;
        use_tmp = true;
    }

    out_fd = open(write_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (out_fd == -1)
    {
        drain_bytes(fd, len);
        return -1;
    }

    remaining = len;
    while (remaining > 0)
    {
        char buf[4096];
        uint32_t chunk;
        int rc;

        chunk = remaining > sizeof(buf) ? sizeof(buf) : remaining;
        rc = read_exact(fd, buf, chunk);
        if (rc <= 0)
        {
            close(out_fd);
            return -1;
        }
        if (writeall(out_fd, buf, chunk) == -1)
        {
            close(out_fd);
            return -1;
        }
        remaining -= chunk;
    }

    close(out_fd);
    if (use_tmp)
    {
        if (chmod(write_path, 0777) == -1)
        {
            unlink(write_path);
            return -1;
        }
        if (rename(write_path, path) == -1)
        {
            unlink(write_path);
            return -1;
        }
        return 0;
    }
    if (is_self_binary_path(path))
    {
        if (chmod(path, 0777) == -1)
        {
            return -1;
        }
    }
    return 0;
}

static int
send_frame(int fd, const void *buf, uint32_t len)
{
    uint8_t header[RMI_FRAME_HEADER_SIZE];

    rmi_write_be32(header, len);
    if (writeall(fd, header, sizeof(header)) == -1)
    {
        return -1;
    }
    if (len == 0)
    {
        return 0;
    }
    return writeall(fd, buf, len);
}

static int
send_text(int fd, const char *text)
{
    size_t len;

    len = strlen(text);
    if (len > UINT32_MAX)
    {
        return -1;
    }
    return send_frame(fd, text, (uint32_t)len);
}

static ssize_t
read_command(int fd, char *buf, size_t size)
{
    uint8_t header[RMI_FRAME_HEADER_SIZE];
    uint32_t len;
    int rc;

    rc = read_exact(fd, header, sizeof(header));
    if (rc <= 0)
    {
        return rc;
    }

    len = rmi_read_be32(header);
    if (len == 0)
    {
        return 0;
    }
    if (len >= size)
    {
        size_t remaining;

        remaining = len;
        while (remaining > 0)
        {
            char tmp[256];
            size_t chunk;
            int rd;

            chunk = remaining > sizeof(tmp) ? sizeof(tmp) : remaining;
            rd = read_exact(fd, tmp, chunk);
            if (rd <= 0)
            {
                return rd;
            }
            remaining -= chunk;
        }
        return -1;
    }

    rc = read_exact(fd, buf, len);
    if (rc <= 0)
    {
        return rc;
    }
    buf[len] = '\0';
    return (ssize_t)len;
}

static int
buffer_append(char **buf, size_t *len, size_t *cap, const char *text, size_t text_len)
{
    size_t needed;
    size_t new_cap;
    char *tmp;

    if (buf == NULL || len == NULL || cap == NULL || text == NULL)
    {
        return -1;
    }
    needed = *len + text_len + 1;
    if (needed > RMI_LIST_MAX_BYTES)
    {
        return -1;
    }
    if (needed > *cap)
    {
        new_cap = *cap == 0 ? 1024 : *cap;
        while (new_cap < needed)
        {
            new_cap *= 2;
            if (new_cap > RMI_LIST_MAX_BYTES)
            {
                new_cap = RMI_LIST_MAX_BYTES;
                if (new_cap < needed)
                {
                    return -1;
                }
                break;
            }
        }
        tmp = (char *)realloc(*buf, new_cap);
        if (tmp == NULL)
        {
            return -1;
        }
        *buf = tmp;
        *cap = new_cap;
    }
    memcpy(*buf + *len, text, text_len);
    *len += text_len;
    (*buf)[*len] = '\0';
    return 0;
}

static int
append_list_line(char **buf, size_t *len, size_t *cap, const char *line)
{
    return buffer_append(buf, len, cap, line, strlen(line));
}

static int
join_path(const char *dir, const char *name, char *out, size_t out_len)
{
    size_t dir_len;

    if (dir == NULL || name == NULL || out == NULL || out_len == 0)
    {
        return -1;
    }
    dir_len = strlen(dir);
    if (dir_len == 0)
    {
        return -1;
    }
    if (dir_len > 1 && dir[dir_len - 1] == '/')
    {
        return snprintf(out, out_len, "%s%s", dir, name) < (int)out_len ? 0 : -1;
    }
    if (strcmp(dir, "/") == 0)
    {
        return snprintf(out, out_len, "/%s", name) < (int)out_len ? 0 : -1;
    }
    return snprintf(out, out_len, "%s/%s", dir, name) < (int)out_len ? 0 : -1;
}

static int
send_file_list(int fd, const char *path)
{
    DIR *dir;
    struct dirent *entry;
    char *buf;
    size_t len;
    size_t cap;
    int rc;

    if (path == NULL || *path == '\0')
    {
        return -1;
    }
    dir = opendir(path);
    if (dir == NULL)
    {
        return -1;
    }
    buf = NULL;
    len = 0;
    cap = 0;

    while ((entry = readdir(dir)) != NULL)
    {
        char full_path[PATH_MAX];
        struct stat st;
        char line[PATH_MAX + 64];
        int line_len;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        {
            continue;
        }
        if (join_path(path, entry->d_name, full_path, sizeof(full_path)) == -1)
        {
            continue;
        }
        if (lstat(full_path, &st) == -1)
        {
            continue;
        }

        if (S_ISDIR(st.st_mode))
        {
            line_len = snprintf(line, sizeof(line), "D\t%s\n", entry->d_name);
        }
        else
        {
            line_len = snprintf(line, sizeof(line), "F\t%s\t%lld\n",
                                entry->d_name,
                                (long long)st.st_size);
        }
        if (line_len < 0 || (size_t)line_len >= sizeof(line))
        {
            continue;
        }
        if (append_list_line(&buf, &len, &cap, line) == -1)
        {
            free(buf);
            closedir(dir);
            return -1;
        }
    }

    closedir(dir);
    if (buf == NULL)
    {
        return send_frame(fd, "", 0);
    }
    rc = send_frame(fd, buf, (uint32_t)len);
    free(buf);
    return rc;
}

static int
send_file_payload(int fd, int file_fd, uint32_t size)
{
    uint8_t header[RMI_FRAME_HEADER_SIZE];
    uint32_t remaining;

    rmi_write_be32(header, size);
    if (writeall(fd, header, sizeof(header)) == -1)
    {
        return -1;
    }
    remaining = size;
    while (remaining > 0)
    {
        char buf[4096];
        ssize_t rd;
        uint32_t chunk;

        chunk = remaining > sizeof(buf) ? sizeof(buf) : remaining;
        rd = read(file_fd, buf, chunk);
        if (rd <= 0)
        {
            return -1;
        }
        if (writeall(fd, buf, (size_t)rd) == -1)
        {
            return -1;
        }
        remaining -= (uint32_t)rd;
    }
    return 0;
}

static int
handle_download(int client_fd, const char *path)
{
    int file_fd;
    struct stat st;

    if (path == NULL || *path == '\0')
    {
        return -1;
    }
    file_fd = open(path, O_RDONLY);
    if (file_fd == -1)
    {
        return -1;
    }
    if (fstat(file_fd, &st) == -1)
    {
        close(file_fd);
        return -1;
    }
    if (!S_ISREG(st.st_mode))
    {
        close(file_fd);
        return -1;
    }
    if (st.st_size < 0 || st.st_size > UINT32_MAX)
    {
        close(file_fd);
        return -1;
    }
    if (send_text(client_fd, RMI_RESP_OK) == -1)
    {
        close(file_fd);
        return -1;
    }
    if (send_file_payload(client_fd, file_fd, (uint32_t)st.st_size) == -1)
    {
        close(file_fd);
        return -1;
    }
    close(file_fd);
    return 0;
}

static int
remove_tree(const char *path)
{
    DIR *dir;
    struct dirent *entry;
    struct stat st;

    if (path == NULL || *path == '\0')
    {
        return -1;
    }
    if (strcmp(path, "/") == 0)
    {
        return -1;
    }
    if (lstat(path, &st) == -1)
    {
        return -1;
    }
    if (S_ISDIR(st.st_mode))
    {
        dir = opendir(path);
        if (dir == NULL)
        {
            return -1;
        }
        while ((entry = readdir(dir)) != NULL)
        {
            char child_path[PATH_MAX];

            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0)
            {
                continue;
            }
            if (join_path(path, entry->d_name, child_path, sizeof(child_path)) == -1)
            {
                closedir(dir);
                return -1;
            }
            if (remove_tree(child_path) == -1)
            {
                closedir(dir);
                return -1;
            }
        }
        closedir(dir);
        return rmdir(path);
    }

    return unlink(path);
}

static int
check_restart_permissions(void)
{
    char path[PATH_MAX];
    struct stat st;
    unsigned int mode;

    snprintf(path, sizeof(path), "%s", "/data/local/tmp/rmi");
    if (stat(path, &st) == -1)
    {
        fprintf(stderr, "RMI restart: stat failed for %s: %d\n", path, errno);
        return -1;
    }
    if (!S_ISREG(st.st_mode))
    {
        fprintf(stderr, "RMI restart: %s is not a regular file.\n", path);
        return -1;
    }
    mode = (unsigned int)(st.st_mode & 0777);
    if (mode != 0777)
    {
        fprintf(stderr, "RMI restart: %s has mode %o, expected 777.\n", path, mode);
        return -1;
    }
    return 0;
}

static void
trim_space(char *s)
{
    size_t len;
    char *start;

    start = s;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
    {
        start++;
    }
    if (start != s)
    {
        memmove(s, start, strlen(start) + 1);
    }

    len = strlen(s);
    while (len > 0)
    {
        char c = s[len - 1];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
        {
            break;
        }
        s[len - 1] = '\0';
        len--;
    }
}

static int
copy_field(char *dst, size_t dst_len, const char *src)
{
    size_t len;

    len = strlen(src);
    if (len == 0 || len >= dst_len)
    {
        return -1;
    }
    snprintf(dst, dst_len, "%s", src);
    return 0;
}

static int
write_default_rmi_config(void)
{
    FILE *fp;

    fp = fopen(RMI_CONFIG_PATH, "w");
    if (fp == NULL)
    {
        fprintf(stderr, "Failed to create default RMI config: %s\n",
                RMI_CONFIG_PATH);
        return -1;
    }
    fprintf(fp, "username=%s\npassword=%s\n", RMI_DEFAULT_USER, RMI_DEFAULT_PASS);
    fclose(fp);
    chmod(RMI_CONFIG_PATH, 0666);
    return 0;
}

static int
load_rmi_config(char *user, size_t user_len, char *pass, size_t pass_len)
{
    FILE *fp;
    char line[256];
    char user_tmp[128];
    char pass_tmp[128];

    user_tmp[0] = '\0';
    pass_tmp[0] = '\0';

    fp = fopen(RMI_CONFIG_PATH, "r");
    if (fp == NULL)
    {
        if (errno == ENOENT)
        {
            if (write_default_rmi_config() == 0)
            {
                if (copy_field(user, user_len, RMI_DEFAULT_USER) == -1 ||
                    copy_field(pass, pass_len, RMI_DEFAULT_PASS) == -1)
                {
                    fprintf(stderr, "Default RMI config fields too long.\n");
                    return -1;
                }
                fprintf(stderr, "Created default RMI config: %s\n", RMI_CONFIG_PATH);
                return 0;
            }
        }
        fprintf(stderr, "RMI config not found: %s\n", RMI_CONFIG_PATH);
        return -1;
    }

    while (fgets(line, sizeof(line), fp) != NULL)
    {
        char *sep;

        trim_space(line);
        if (line[0] == '\0' || line[0] == '#')
        {
            continue;
        }

        if (strncmp(line, "username=", 9) == 0)
        {
            char *val = line + 9;
            trim_space(val);
            if (copy_field(user_tmp, sizeof(user_tmp), val) == -1)
            {
                fclose(fp);
                return -1;
            }
            continue;
        }
        if (strncmp(line, "password=", 9) == 0)
        {
            char *val = line + 9;
            trim_space(val);
            if (copy_field(pass_tmp, sizeof(pass_tmp), val) == -1)
            {
                fclose(fp);
                return -1;
            }
            continue;
        }

        if (user_tmp[0] == '\0' && pass_tmp[0] == '\0')
        {
            sep = strchr(line, ':');
            if (sep == NULL)
            {
                sep = strpbrk(line, " \t");
            }
            if (sep != NULL)
            {
                *sep = '\0';
                sep++;
                trim_space(line);
                trim_space(sep);
                if (copy_field(user_tmp, sizeof(user_tmp), line) == -1 ||
                    copy_field(pass_tmp, sizeof(pass_tmp), sep) == -1)
                {
                    fclose(fp);
                    return -1;
                }
                continue;
            }

            if (copy_field(user_tmp, sizeof(user_tmp), line) == -1)
            {
                fclose(fp);
                return -1;
            }
            continue;
        }

        if (pass_tmp[0] == '\0')
        {
            if (copy_field(pass_tmp, sizeof(pass_tmp), line) == -1)
            {
                fclose(fp);
                return -1;
            }
            continue;
        }
    }

    fclose(fp);

    if (user_tmp[0] == '\0' || pass_tmp[0] == '\0')
    {
        fprintf(stderr, "RMI config missing username/password.\n");
        return -1;
    }
    if (copy_field(user, user_len, user_tmp) == -1 ||
        copy_field(pass, pass_len, pass_tmp) == -1)
    {
        fprintf(stderr, "RMI config fields too long.\n");
        return -1;
    }
    return 0;
}

static int
send_screencap(int client_fd)
{
    int pipefd[2];
    char buf[4096];
    char *data;
    size_t size;
    size_t cap;
    pid_t pid;
    ssize_t n;
    int status;

    data = NULL;
    size = 0;
    cap = 0;

    if (pipe(pipefd) == -1)
    {
        fprintf(stderr, "Syscall error: pipe at line %d with code %d.\n",
                __LINE__, errno);
        return -1;
    }

    pid = fork();
    if (pid == -1)
    {
        fprintf(stderr, "Syscall error: fork at line %d with code %d.\n",
                __LINE__, errno);
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0)
    {
        if (dup2(pipefd[1], STDOUT_FILENO) == -1)
        {
            _exit(127);
        }
        close(pipefd[0]);
        close(pipefd[1]);
        execl("/system/bin/screencap", "screencap", "-p", (char *)NULL);
        _exit(127);
    }

    close(pipefd[1]);
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0)
    {
        size_t needed;

        needed = size + (size_t)n;
        if (needed > UINT32_MAX)
        {
            free(data);
            close(pipefd[0]);
            waitpid(pid, &status, 0);
            return -1;
        }
        if (needed > cap)
        {
            size_t next;
            char *tmp;

            next = cap == 0 ? 8192 : cap * 2;
            while (next < needed)
            {
                next *= 2;
            }
            tmp = realloc(data, next);
            if (tmp == NULL)
            {
                free(data);
                close(pipefd[0]);
                waitpid(pid, &status, 0);
                return -1;
            }
            data = tmp;
            cap = next;
        }
        memcpy(data + size, buf, (size_t)n);
        size = needed;
    }
    if (n == -1)
    {
        free(data);
        close(pipefd[0]);
        waitpid(pid, &status, 0);
        return -1;
    }
    close(pipefd[0]);
    waitpid(pid, &status, 0);

    if (send_frame(client_fd, data, (uint32_t)size) == -1)
    {
        free(data);
        return -1;
    }

    free(data);
    return 0;
}

static int
send_keyevent(int keycode)
{
    struct input_event events[4];
    struct timeval now;
    int fd;
    const char *path;

    if (keycode < 0 || keycode > KEY_MAX)
    {
        return -1;
    }

    path = "/dev/input/event2";
    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd == -1)
    {
        return -1;
    }

    fprintf(stderr, "RMI keyevent: using %s for keycode %d\n", path, keycode);

    if (gettimeofday(&now, NULL) == -1)
    {
        close(fd);
        return -1;
    }

    events[0].time = now;
    events[0].type = EV_KEY;
    events[0].code = (unsigned short)keycode;
    events[0].value = 1;

    events[1].time = now;
    events[1].type = EV_SYN;
    events[1].code = SYN_REPORT;
    events[1].value = 0;

    events[2].time = now;
    events[2].type = EV_KEY;
    events[2].code = (unsigned short)keycode;
    events[2].value = 0;

    events[3].time = now;
    events[3].type = EV_SYN;
    events[3].code = SYN_REPORT;
    events[3].value = 0;

    if (writeall(fd, events, sizeof(events)) == -1)
    {
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

static int
send_keyevent_input(int keycode)
{
    pid_t pid;
    int status;
    char key_str[16];
    int sh_ok;
    int runcon_ok;
    int app_process_ok;
    int app_process64_ok;
    int app_process32_ok;
    int cmd_ok;
    int toolbox_ok;
    int toybox_ok;

    if (keycode < 0)
    {
        return -1;
    }
    if (snprintf(key_str, sizeof(key_str), "%d", keycode) >= (int)sizeof(key_str))
    {
        return -1;
    }

    fprintf(stderr, "RMI press_input: keycode %d\n", keycode);

    sh_ok = (access("/system/bin/sh", X_OK) == 0);
    runcon_ok = (access("/system/bin/runcon", X_OK) == 0);
    app_process_ok = (access("/system/bin/app_process", X_OK) == 0);
    app_process64_ok = (access("/system/bin/app_process64", X_OK) == 0);
    app_process32_ok = (access("/system/bin/app_process32", X_OK) == 0);
    cmd_ok = (access("/system/bin/cmd", X_OK) == 0);
    toolbox_ok = (access("/system/bin/toolbox", X_OK) == 0);
    toybox_ok = (access("/system/bin/toybox", X_OK) == 0);

    pid = fork();
    if (pid == -1)
    {
        fprintf(stderr, "Syscall error: fork at line %d with code %d.\n",
                __LINE__, errno);
        return -1;
    }

    if (pid == 0)
    {
        set_shell_env();
        log_identity("before runcon");
        if (runcon_ok)
        {
            fprintf(stderr, "RMI press_input: exec /system/bin/runcon shell\n");
            execl("/system/bin/runcon", "runcon", "u:r:shell:s0",
                  "/system/bin/sh", "/system/bin/input", "keyevent",
                  key_str, (char *)NULL);
            fprintf(stderr, "RMI press_input: exec /system/bin/runcon failed: %d\n",
                    errno);
        }
        drop_to_shell_user();
        log_identity("after drop");
        if (sh_ok)
        {
            fprintf(stderr, "RMI press_input: exec /system/bin/sh /system/bin/input\n");
            execl("/system/bin/sh", "sh", "/system/bin/input", "keyevent",
                  key_str, (char *)NULL);
            fprintf(stderr, "RMI press_input: exec sh /system/bin/input failed: %d\n",
                    errno);
        }
        if (app_process_ok)
        {
            if (setenv("CLASSPATH", "/system/framework/input.jar", 1) == 0)
            {
                fprintf(stderr, "RMI press_input: exec /system/bin/app_process\n");
                execl("/system/bin/app_process", "app_process", "/system/bin",
                      "com.android.commands.input.Input", "keyevent", key_str,
                      (char *)NULL);
                fprintf(stderr, "RMI press_input: exec /system/bin/app_process failed: %d\n",
                        errno);
            }
            else
            {
                fprintf(stderr, "RMI press_input: setenv CLASSPATH failed: %d\n",
                        errno);
            }
        }
        if (app_process64_ok)
        {
            if (setenv("CLASSPATH", "/system/framework/input.jar", 1) == 0)
            {
                fprintf(stderr, "RMI press_input: exec /system/bin/app_process64\n");
                execl("/system/bin/app_process64", "app_process64", "/system/bin",
                      "com.android.commands.input.Input", "keyevent", key_str,
                      (char *)NULL);
                fprintf(stderr, "RMI press_input: exec /system/bin/app_process64 failed: %d\n",
                        errno);
            }
            else
            {
                fprintf(stderr, "RMI press_input: setenv CLASSPATH failed: %d\n",
                        errno);
            }
        }
        if (app_process32_ok)
        {
            if (setenv("CLASSPATH", "/system/framework/input.jar", 1) == 0)
            {
                fprintf(stderr, "RMI press_input: exec /system/bin/app_process32\n");
                execl("/system/bin/app_process32", "app_process32", "/system/bin",
                      "com.android.commands.input.Input", "keyevent", key_str,
                      (char *)NULL);
                fprintf(stderr, "RMI press_input: exec /system/bin/app_process32 failed: %d\n",
                        errno);
            }
            else
            {
                fprintf(stderr, "RMI press_input: setenv CLASSPATH failed: %d\n",
                        errno);
            }
        }
        if (cmd_ok)
        {
            fprintf(stderr, "RMI press_input: exec /system/bin/cmd\n");
            execl("/system/bin/cmd", "cmd", "input", "keyevent", key_str,
                  (char *)NULL);
            fprintf(stderr, "RMI press_input: exec /system/bin/cmd failed: %d\n",
                    errno);
        }
        if (toybox_ok)
        {
            fprintf(stderr, "RMI press_input: exec /system/bin/toybox\n");
            execl("/system/bin/toybox", "toybox", "input", "keyevent", key_str,
                  (char *)NULL);
            fprintf(stderr, "RMI press_input: exec /system/bin/toybox failed: %d\n",
                    errno);
        }
        if (toolbox_ok)
        {
            fprintf(stderr, "RMI press_input: exec /system/bin/toolbox\n");
            execl("/system/bin/toolbox", "toolbox", "input", "keyevent", key_str,
                  (char *)NULL);
            fprintf(stderr, "RMI press_input: exec /system/bin/toolbox failed: %d\n",
                    errno);
        }
        _exit(127);
    }

    if (waitpid(pid, &status, 0) == -1)
    {
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        if (WIFEXITED(status))
        {
            fprintf(stderr, "RMI press_input: exit status %d\n",
                    WEXITSTATUS(status));
        }
        else if (WIFSIGNALED(status))
        {
            fprintf(stderr, "RMI press_input: signaled %d\n",
                    WTERMSIG(status));
        }
        return -1;
    }

    return 0;
}

static int
handle_rmi_client(int client_fd, const char *user, const char *pass)
{
    struct pollfd pfd;
    char cmd[1024];
    ssize_t n;
    int attempts;
    bool authed;

    pfd.fd = client_fd;
    pfd.events = POLLIN;
    attempts = 0;
    authed = false;

    while (1)
    {
        int pr;

        pr = poll(&pfd, 1, RMI_HEARTBEAT_MS);
        if (pr == 0)
        {
            if (send_text(client_fd, RMI_CMD_HEARTBEAT) == -1)
            {
                return RMI_CONTINUE;
            }
            continue;
        }
        if (pr == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }
            fprintf(stderr, "Syscall error: poll at line %d with code %d.\n",
                    __LINE__, errno);
            return RMI_CONTINUE;
        }
        if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL))
        {
            return RMI_CONTINUE;
        }
        if (!(pfd.revents & POLLIN))
        {
            continue;
        }

        n = read_command(client_fd, cmd, sizeof(cmd));
        if (n <= 0)
        {
            return RMI_CONTINUE;
        }
        if (cmd[0] == '\0')
        {
            continue;
        }

        if (!authed)
        {
            char *save;
            char *tok;
            char *u;
            char *p;

            tok = strtok_r(cmd, " \t", &save);
            if (tok != NULL && strcmp(tok, RMI_CMD_AUTH) == 0)
            {
                u = strtok_r(NULL, " \t", &save);
                p = strtok_r(NULL, " \t", &save);
                if (u != NULL && p != NULL &&
                    strcmp(u, user) == 0 && strcmp(p, pass) == 0)
                {
                    if (send_text(client_fd, RMI_RESP_OK) == -1)
                    {
                        return RMI_CONTINUE;
                    }
                    authed = true;
                    continue;
                }
            }

            attempts++;
            if (attempts >= 3)
            {
                send_text(client_fd, "ERR auth failed");
                return RMI_CONTINUE;
            }

            send_text(client_fd, "ERR auth required");
            continue;
        }

        if (strcmp(cmd, RMI_CMD_QUIT) == 0)
        {
            send_text(client_fd, RMI_RESP_OK);
            return RMI_SHUTDOWN;
        }

        if (strcmp(cmd, RMI_CMD_RESTART) == 0)
        {
            if (check_restart_permissions() == -1)
            {
                send_text(client_fd, "ERR restart");
                continue;
            }
            send_text(client_fd, RMI_RESP_OK);
            return RMI_RESTART;
        }

        if (strcmp(cmd, RMI_CMD_VERSION) == 0)
        {
            char msg[64];

            if (snprintf(msg, sizeof(msg), "%s%u",
                         RMI_RESP_VERSION_PREFIX,
                         (unsigned int)RMI_VERSION) >= (int)sizeof(msg))
            {
                send_text(client_fd, "ERR version");
            }
            else
            {
                send_text(client_fd, msg);
            }
            continue;
        }

        if (strcmp(cmd, RMI_CMD_HEARTBEAT) == 0)
        {
            send_text(client_fd, RMI_RESP_OK);
            continue;
        }

        if (strncmp(cmd, RMI_CMD_PRESS_INPUT, strlen(RMI_CMD_PRESS_INPUT)) == 0)
        {
            char *save;
            char *tok;
            char *code_str;
            char *end;
            long code;

            tok = strtok_r(cmd, " \t", &save);
            code_str = strtok_r(NULL, " \t", &save);
            if (tok != NULL && code_str != NULL)
            {
                errno = 0;
                code = strtol(code_str, &end, 10);
                if (errno == 0 && end != code_str && *end == '\0')
                {
                    if (send_keyevent_input((int)code) == 0)
                    {
                        send_text(client_fd, RMI_RESP_OK);
                        continue;
                    }
                    send_text(client_fd, "ERR press");
                    continue;
                }
            }
            send_text(client_fd, "ERR press");
            continue;
        }

        if (strncmp(cmd, RMI_CMD_UPLOAD, strlen(RMI_CMD_UPLOAD)) == 0)
        {
            char *save;
            char *tok;
            char *path;
            char *size_str;
            char *end;
            unsigned long size;

            tok = strtok_r(cmd, " \t", &save);
            path = strtok_r(NULL, " \t", &save);
            size_str = strtok_r(NULL, " \t", &save);
            if (tok != NULL && path != NULL && size_str != NULL)
            {
                errno = 0;
                size = strtoul(size_str, &end, 10);
                if (errno == 0 && end != size_str && *end == '\0' &&
                    size <= UINT32_MAX)
                {
                    if (recv_frame_to_file(client_fd, path, (uint32_t)size) == 0)
                    {
                        send_text(client_fd, RMI_RESP_OK);
                        continue;
                    }
                    send_text(client_fd, "ERR upload");
                    continue;
                }
            }
            send_text(client_fd, "ERR upload");
            continue;
        }

        if (strncmp(cmd, RMI_CMD_LIST, strlen(RMI_CMD_LIST)) == 0)
        {
            char *save;
            char *tok;
            char *path;

            tok = strtok_r(cmd, " \t", &save);
            path = strtok_r(NULL, " \t", &save);
            if (tok != NULL && path != NULL)
            {
                if (send_file_list(client_fd, path) == 0)
                {
                    continue;
                }
            }
            send_text(client_fd, "ERR list");
            continue;
        }

        if (strncmp(cmd, RMI_CMD_DOWNLOAD, strlen(RMI_CMD_DOWNLOAD)) == 0)
        {
            char *save;
            char *tok;
            char *path;

            tok = strtok_r(cmd, " \t", &save);
            path = strtok_r(NULL, " \t", &save);
            if (tok != NULL && path != NULL)
            {
                if (handle_download(client_fd, path) == 0)
                {
                    continue;
                }
            }
            send_text(client_fd, "ERR download");
            continue;
        }

        if (strncmp(cmd, RMI_CMD_DELETE, strlen(RMI_CMD_DELETE)) == 0)
        {
            char *save;
            char *tok;
            char *path;

            tok = strtok_r(cmd, " \t", &save);
            path = strtok_r(NULL, " \t", &save);
            if (tok != NULL && path != NULL)
            {
                if (remove_tree(path) == 0)
                {
                    send_text(client_fd, RMI_RESP_OK);
                    continue;
                }
            }
            send_text(client_fd, "ERR delete");
            continue;
        }

        if (strncmp(cmd, RMI_CMD_PRESS, strlen(RMI_CMD_PRESS)) == 0)
        {
            char *save;
            char *tok;
            char *code_str;
            char *end;
            long code;

            tok = strtok_r(cmd, " \t", &save);
            code_str = strtok_r(NULL, " \t", &save);
            if (tok != NULL && code_str != NULL)
            {
                errno = 0;
                code = strtol(code_str, &end, 10);
                if (errno == 0 && end != code_str && *end == '\0')
                {
                    if (send_keyevent((int)code) == 0)
                    {
                        send_text(client_fd, RMI_RESP_OK);
                        continue;
                    }
                    send_text(client_fd, "ERR press");
                    continue;
                }
            }
            send_text(client_fd, "ERR press");
            continue;
        }

        if (strcmp(cmd, RMI_CMD_SCREENCAP) == 0)
        {
            if (send_screencap(client_fd) == -1)
            {
                send_text(client_fd, "ERR screencap");
            }
            continue;
        }

        send_text(client_fd, "ERR unknown command");
    }

    return RMI_CONTINUE;
}

static void
rmi_server(uint16_t port)
{
    struct sockaddr_in addr;
    socklen_t addr_len;
    char user[128];
    char pass[128];
    int s;

    if (load_rmi_config(user, sizeof(user), pass, sizeof(pass)) == -1)
    {
        exit(EXIT_FAILURE);
    }

    s = setup_socket(htons(port));

    printf(">>> RMI command server listening on 0.0.0.0:%u\n\n", port);

    while (1)
    {
        int c;

        addr_len = sizeof(addr);
        c = accept(s, (struct sockaddr *)&addr, &addr_len);
        if (c == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }
            fprintf(stderr, "Syscall error: accept at line %d with code %d.\n",
                    __LINE__, errno);
            break;
        }

        int result;

        result = handle_rmi_client(c, user, pass);
        close(c);
        if (result == RMI_SHUTDOWN)
        {
            break;
        }
        if (result == RMI_RESTART)
        {
            close(s);
            if (rmi_argv == NULL || rmi_argv[0] == NULL)
            {
                fprintf(stderr, "RMI restart failed: missing argv.\n");
                exit(EXIT_FAILURE);
            }
            execv(rmi_argv[0], rmi_argv);
            fprintf(stderr, "Syscall error: execv at line %d with code %d.\n",
                    __LINE__, errno);
            exit(EXIT_FAILURE);
        }
    }

    close(s);
}

int
rmi(int argc, char *argv[])
{
    uint16_t port;

    redirect_rmi_logs();
    rmi_argv = argv;
    port = DEFAULT_PORT;
    if (argc > 3)
    {
        fprintf(stderr, "Command line error: too many options.\n");
        exit(EXIT_FAILURE);
    }
    if (argc > 1)
    {
        port = (uint16_t)atoi(argv[argc - 1]);
        if (port == 0)
        {
            fprintf(stderr, "Command line error: invalid port.\n");
            exit(EXIT_FAILURE);
        }
    }

    rmi_server(port);
    return EXIT_SUCCESS;
}
