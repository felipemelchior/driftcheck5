/*
 * DriftCheck - installer payload.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Sent to the ELF loader on port 9021, this payload writes the DriftCheck app
 * folder, which is embedded in it, to /data/homebrew/<TITLE_ID>/, and
 * ShadowMountPlus puts the icon on the home screen on its next scan.
 *
 * Every file and folder is left at 0777. The console only starts an app whose
 * files are open to all and answers CE-107750-0 otherwise; some FTP servers
 * store uploads as 0666, so installing through this payload avoids that.
 *
 * Each file is written beside its destination and renamed over it only when
 * complete, so an interrupted update never leaves a half-written eboot.bin.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "installer_files.h"

typedef struct
{
    char unused[45];
    char message[3075];
} notify_request;

int sceKernelSendNotificationRequest(int, notify_request *, size_t, int);

static void notify(const char *format, ...)
{
    notify_request request;
    va_list args;

    memset(&request, 0, sizeof(request));
    va_start(args, format);
    vsnprintf(request.message, sizeof(request.message), format, args);
    va_end(args);
    sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
    printf("%s\n", request.message);
}

/* mkdir -p for the parents of path. Only the folders inside the app's own folder
 * are opened to 0777; /data and /data/homebrew keep the modes they have. */
static int make_parents(const char *path)
{
    char partial[512];
    size_t length = strlen(path);

    if (length >= sizeof(partial))
        return -1;
    memcpy(partial, path, length + 1);
    for (char *slash = partial + 1; (slash = strchr(slash, '/')) != NULL; slash++)
    {
        *slash = '\0';
        if (mkdir(partial, 0777) != 0 && errno != EEXIST)
            return -1;
        if (strncmp(partial, INSTALLER_DESTINATION, strlen(INSTALLER_DESTINATION)) == 0)
            chmod(partial, 0777);
        *slash = '/';
    }
    return 0;
}

static int write_file(const char *path, const unsigned char *data, size_t size)
{
    char temporary[512];
    int descriptor;
    size_t done = 0;

    if (snprintf(temporary, sizeof(temporary), "%s.installing", path) >= (int)sizeof(temporary))
        return -1;
    if (make_parents(path) != 0)
        return -1;
    descriptor = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0777);
    if (descriptor < 0)
        return -1;
    while (done < size)
    {
        ssize_t written = write(descriptor, data + done, size - done);
        if (written <= 0)
        {
            close(descriptor);
            unlink(temporary);
            return -1;
        }
        done += (size_t)written;
    }
    if (fsync(descriptor) != 0 || close(descriptor) != 0)
    {
        unlink(temporary);
        return -1;
    }
    /* open() is subject to the umask; the mode is set explicitly. */
    if (chmod(temporary, 0777) != 0 || rename(temporary, path) != 0)
    {
        unlink(temporary);
        return -1;
    }
    return 0;
}

int main(void)
{
    char path[512];

    for (size_t i = 0; i < installer_file_count; i++)
    {
        const installer_file *file = &installer_files[i];
        snprintf(path, sizeof(path), "%s/%s", INSTALLER_DESTINATION, file->name);
        if (write_file(path, file->start, (size_t)(file->end - file->start)) != 0)
        {
            notify("DriftCheck: install failed at %s (%s)", file->name, strerror(errno));
            return 1;
        }
    }
    chmod(INSTALLER_DESTINATION, 0777);
    notify("DriftCheck %s installed. The icon appears on the home screen within a few "
           "seconds (needs ShadowMountPlus).",
           INSTALLER_VERSION);
    return 0;
}
