// SE370 - Assignment 2 - Memory File System
// All work is done by Ernest Pui Hong Wong.
// UPI: ewon746
// ID: 178687569


#define FUSE_USE_VERSION 31
#define _POSIX_C_SOURCE 200809L

#include "eduFUSE/edufuse.h"
#include <fuse.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>



//
// Helpers
//



static int copy_string(char *dest, const char *src, size_t length) {
    strncpy(dest, src, length);
    int too_long = dest[length - 1];
    dest[length - 1] = '\0';
    return too_long ? -ENAMETOOLONG : 0;
}



//
// Parameters
//




#define RENAME_NOREPLACE (1 << 0)
#define RENAME_EXCHANGE (1 << 1)

static const char *filepath = "/hello";
static const char *filename = "hello";
static const char *filecontent = "Hello World!\n";

enum { FILENAME_SIZE = 256 };
enum { PATH_SIZE = 4096 };
enum { BLOCK_SIZE = 512 };



//
// Structures
//



typedef struct MemoryInode MemoryInode;
typedef struct DirEntry DirEntry;
typedef struct DirContents DirContents;
typedef struct FileContents FileContents;

/**
 * Implement directory entries as a linked list.
 * It is probably better if it was using some sort of tree, but
 * linked lists will work for the scale of this assignment.
 */
struct DirEntry {
    char name[FILENAME_SIZE];
    MemoryInode *inode;
    DirEntry *previous;
    DirEntry *next;
};

struct DirContents {
    DirEntry entry; // Linked list's head is '.'
    DirEntry parent; // Linked list's tail is '..'
};

struct FileContents {
    void *data;
    size_t buffer_size;
};

struct MemoryInode {
    struct stat stat;
    int open_count;
    union {
        FileContents file;
        DirContents dir;
        char * symlink;
    } contents;
};



//
// Inode operations.
//



/**
 * The root inode ("/") of our filesystem.
 */
static MemoryInode * root;

/**
 * The inode numbers globally increment.
 */
static unsigned long ino_number = 0;

/**
 * Create a new generic inode.
 * Prefill the necessary stat properties here.
 */
static MemoryInode *new_inode(void) {
    MemoryInode *inode = calloc(1, sizeof(MemoryInode));
    inode->stat.st_ino = ino_number++;
    inode->stat.st_blksize = BLOCK_SIZE;
    inode->stat.st_uid = getuid();
    inode->stat.st_gid = getgid();
    time(&inode->stat.st_atime);
    time(&inode->stat.st_mtime);
    time(&inode->stat.st_ctime);
    return inode;
}

/**
 * Create a directory inode that is not linked yet.
 * Directories are linked to themselves at first, as is
 * the case for the root inode. As a useful side-effect,
 * the nlink count for the root is correctly 2.
 */
static MemoryInode *new_dir_inode(void) {
    MemoryInode *dir = new_inode();
    dir->stat.st_mode = S_IFDIR | 0755;

    // Link to itself.
    dir->stat.st_nlink++;
    strcpy(dir->contents.dir.entry.name, ".");
    dir->contents.dir.entry.inode = dir;
    dir->contents.dir.entry.previous = NULL;
    dir->contents.dir.entry.next = &dir->contents.dir.parent;

    // Link to parent.
    dir->stat.st_nlink++;
    strcpy(dir->contents.dir.parent.name, "..");
    dir->contents.dir.parent.inode = dir;
    dir->contents.dir.parent.previous = &dir->contents.dir.entry;
    dir->contents.dir.parent.next = NULL;

    return dir;
}

/**
 * Create a regular (file) inode.
 */
static MemoryInode *new_reg_inode(void) {
    MemoryInode *inode = new_inode();
    inode->stat.st_mode = S_IFREG | 0777;
    return inode;
}

/**
 * Helper to refresh the dummy st_blocks size.
 * Note: the block size for this stat field is defined
 * to be 512 bytes irrespective of st_blksize.
 */
static void inode_update_blocks(MemoryInode *inode) {
    inode->stat.st_blocks = (inode->stat.st_size + BLOCK_SIZE - 1) / BLOCK_SIZE;
}

/**
 * Returns a truthy value iff the inode is a directory.
 */
static int is_dir(MemoryInode *inode) {
    return S_ISDIR(inode->stat.st_mode);
}

/**
 * Returns a truthy value iff the directory is empty.
 */
static int is_dir_empty(MemoryInode *dir) {
    assert(is_dir(dir));
    // Check if there is some entry other than the
    // two default "." and ".." entries.
    return dir->contents.dir.entry.next->next == NULL;
}


/**
 * Lower level linking of an inode to a directory.
 * Adds an entry to the directory's linked list.
 * If the entry is for a directory, the child's ".." is also
 * linked to the parent.
 */
static void direntry_add(DirEntry *entry, MemoryInode *parent) {
    // Link after the "." entry.
    if (parent->contents.dir.entry.next) {
        parent->contents.dir.entry.next->previous = entry;
    }
    entry->previous = &parent->contents.dir.entry;
    entry->next = parent->contents.dir.entry.next;
    parent->contents.dir.entry.next = entry;

    // Housekeeping.
    entry->inode->stat.st_nlink++;
    parent->stat.st_size += sizeof(DirEntry);
    inode_update_blocks(parent);

    // Update ".." for directories
    if (is_dir(entry->inode)) {
        parent->stat.st_nlink++;
        entry->inode->stat.st_nlink--;
        entry->inode->contents.dir.parent.inode = parent;
    }
}

/**
 * Lower level unlinking and freeing of a directory entry.
 * Note: must not be called on the default "." and ".." entries.
 * Also unlinks the ".." if the entry is for a child directory.
 */
static void direntry_remove(DirEntry *entry, MemoryInode *parent) {
    // Only the "." entry has a NULL previous.
    assert(entry->previous);
    // Only the ".." entry has a NULL next.
    assert(entry->next);

    // Bypass the entry.
    entry->previous->next = entry->next;
    entry->next->previous = entry->previous;

    // Housekeeping.
    entry->inode->stat.st_nlink--;
    parent->stat.st_size -= sizeof(DirEntry);
    inode_update_blocks(parent);

    // Update ".." for directories
    if (is_dir(entry->inode)) {
        parent->stat.st_nlink--;
        entry->inode->stat.st_nlink++;
        entry->inode->contents.dir.parent.inode = entry->inode;
    }

    free(entry);
}

/**
 * Higher level linking of an inode by a name.
 */
static int inode_link(MemoryInode *parent, MemoryInode *child, const char *name) {
    if (!is_dir(parent)) {
        return -ENOTDIR;
    }

    // Create and init new entry.
    DirEntry *entry = malloc(sizeof(DirEntry));
    int error = copy_string(entry->name, name, FILENAME_SIZE);
    if (error) {
        free(entry);
        return error;
    }
    entry->inode = child;

    direntry_add(entry, parent);
    time(&parent->stat.st_mtime);
    return 0;
}

/**
 * Free the inode when it is no longer linked and
 * no more processes have it opened.
 */
static void inode_delete(MemoryInode *inode) {
    if (is_dir(inode)) {
        // Nothing to free.
    } else if (S_ISLNK(inode->stat.st_mode)) {
        free(inode->contents.symlink);
    } else {
        free(inode->contents.file.data);
    }
    free(inode);
}

/**
 * Higher level unlinking of an inode, and frees inode if fully unlinked.
 */
static void inode_unlink(DirEntry *entry, MemoryInode *parent) {
    // Count remaining references.
    // Directories are always deleted.
    const int references =
        !is_dir(entry->inode) && entry->inode->stat.st_nlink;

    // If we are unlinking a directory, it should be empty by now.
    assert(!is_dir(entry->inode) || is_dir_empty(entry->inode));

    if (!references && !entry->inode->open_count) {
        inode_delete(entry->inode);
    }

    // Note: free entry only after freeing inode.
    // Don't refer to entry->inode after freeing entry.
    direntry_remove(entry, parent);
    time(&parent->stat.st_mtime);
}

/**
 * Change the size of an inode, reallocating the buffer if necessary.
 */
static int inode_resize(MemoryInode *inode, size_t size) {
    if (is_dir(inode)) {
        return -EISDIR;
    }

    const size_t old_length = inode->stat.st_size;

    if (size > inode->contents.file.buffer_size) {
        // Preallocate more than needed to avoid excessive reallocations.
        size_t new_buffer_size = size * 2;
        inode->contents.file.data = realloc(inode->contents.file.data, new_buffer_size);
        inode->contents.file.buffer_size = new_buffer_size;
    } else if (size < old_length) {
        // Truncate.
        memset(inode->contents.file.data, 0, old_length - size);
    }
    inode->stat.st_size = size;
    time(&inode->stat.st_mtime);
    inode_update_blocks(inode);

    return 0;
}

/**
 * Write to an inode.
 */
static int inode_write(MemoryInode *inode, const char *data, size_t length, size_t offset) {
    if (is_dir(inode)) {
        return -EISDIR;
    }

    if (offset + length > inode->stat.st_size) {
        int error = inode_resize(inode, offset + length);
        if (error) return error;
    }

    memcpy(inode->contents.file.data + offset, data, length);

    return length;
}

/**
 * Loop through all directory entries in a directory.
 * Well, self explanatory.
 */
#define FOREACH_DIRENTRY(inode, entry) \
    for (DirEntry *entry = &inode->contents.dir.entry; \
            entry != NULL; \
            entry = entry->next)

/**
 * Find a child direntry with the given filename directly inside the given directory,
 * and point the result to the direntry, or NULL if not found.
 * Returns:
 * 0 on success,
 * ENOENT when entry is not found
 * ENOTDIR when the directory inode given is not a directory.
 */
static int find_child_entry(const char *name, MemoryInode *dir, DirEntry **result) {
    assert(name != NULL && dir != NULL);
    if (!is_dir(dir)) {
        *result = NULL;
        return -ENOTDIR;
    }

    FOREACH_DIRENTRY(dir, entry) {
        if (strncmp(name, entry->name, sizeof(entry->name)) == 0) {
            assert(entry->inode != NULL);
            *result = entry;
            return 0;
        }
    }

    *result = NULL;
    return -ENOENT;
}

/**
 * Find a child file with the given filename directly inside the given directory,
 * and point the result to the inode, or NULL if not found.
 * Returns:
 * 0 on success,
 * ENOENT when entry is not found
 * ENOTDIR when the directory inode given is not a directory.
 */
static int find_child(const char *name, MemoryInode *dir, MemoryInode **result) {
    DirEntry * entry;
    int error = find_child_entry(name, dir, &entry);
    *result = entry ? entry->inode : NULL;
    return error;
}

/**
 * Find an inode using a path relative to the given root inode, and point the
 * result to the inode, or NULL if not found.
 * Returns:
 * 0 on success,
 * ENOENT when entry is not found
 * ENOENT when a directory in the path is not found.
 * ENOTDIR when the path contains components that are not directories.
 */
static int find_from_path(const char *path, MemoryInode *root, MemoryInode **result) {
    assert(path != NULL && root != NULL);

    // strtok is destructive, so copy onto buffer first.
    char tokenized[PATH_SIZE];
    copy_string(tokenized, path, sizeof(tokenized));

    char *name = strtok(tokenized, "/");
    MemoryInode *inode = root;
    while (name != NULL && inode != NULL) {
        int error = find_child(name, inode, &inode);
        if (error) return error;
        name = strtok(NULL, "/");
    }

    if (name != NULL || inode == NULL) {
        // Full path was not resolved.
        *result = NULL;
        return -ENOENT;
    }

    *result = inode;
    return 0;
}

/**
 * Take a path, and extract the filename and the parent directory path
 * by placing a null character at the last slash.
 * The original path string will then become the parent directory path.
 * A pointer to the position where the filename starts is returned.
 * Assumes that there is at least one slash in the path.
 */
static char *split_basename(char *path) {
    char *last_delim = path;
    for (char *ptr = path; *ptr != '\0'; ptr++) {
        if (*ptr == '/') {
            last_delim = ptr;
        }
    }
    assert(*last_delim == '/');
    *last_delim = '\0';
    return last_delim + 1;
}



//
// FUSE bindings
//



static int mfs_getattr(const char *path, struct stat *stbuf) {
    MemoryInode *inode;
    int error = find_from_path(path, root, &inode);
    if (error) return error;
    memcpy(stbuf, &inode->stat, sizeof(struct stat));
    return 0;
}

static int mfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                           off_t offset, struct fuse_file_info *fi) {
    (void) offset;
    (void) fi;

    MemoryInode *inode;
    int error = find_from_path(path, root, &inode);
    if (error) return error;

    if (!is_dir(inode)) return -EISDIR;

    time(&inode->stat.st_atime);

    FOREACH_DIRENTRY(inode, entry) {
        filler(buf, entry->name, NULL, 0);
    }

    return 0;
}

static int mfs_utimens(const char *path, const struct timespec tv[2],
        struct fuse_file_info *fi) {
    (void) fi;

    MemoryInode *inode;
    int error = find_from_path(path, root, &inode);
    if (error) return error;
    inode->stat.st_atime = tv[0].tv_sec;
    inode->stat.st_mtime = tv[1].tv_sec;
    time(&inode->stat.st_ctime);
    return 0;
}

static int mfs_open(const char *path, struct fuse_file_info *fi) {
    (void) fi;

    MemoryInode *inode;
    int error = find_from_path(path, root, &inode);
    if (error) return error;
    inode->open_count++;
    return 0;
}

static int mfs_release(const char *path, struct fuse_file_info *fi) {
    (void) fi;

    MemoryInode *inode;
    int error = find_from_path(path, root, &inode);
    if (error) return error;
    inode->open_count--;

    if (inode->open_count > 0) return 0;

    if (is_dir(inode)) {
        if (inode->stat.st_nlink == 1) {
            inode_delete(inode);
        }
    } else if (inode->stat.st_nlink == 0) {
        inode_delete(inode);
    }

    return 0;
}

static int mfs_read(const char *path, char *buf, size_t size, off_t offset,
        struct fuse_file_info *fi) {
    (void) fi;

    MemoryInode *inode;
    int error = find_from_path(path, root, &inode);
    if (error) return error;
    if (is_dir(inode)) return -EISDIR;
    if (S_ISLNK(inode->stat.st_mode)) return -EINVAL;

    time(&inode->stat.st_atime);

    size_t length = inode->stat.st_size;
    if (offset >= length) length = 0;
    else length -= offset;
    if (length > size) length = size;

    memcpy(buf, inode->contents.file.data, length);
    memset(buf + length, 0, size - length);

    return length;
}

static int mfs_write(const char *path, const char *data, size_t size, off_t offset,
        struct fuse_file_info *fi) {
    (void) fi;

    MemoryInode *inode;
    int error = find_from_path(path, root, &inode);
    if (error) return error;
    if (is_dir(inode)) return -EISDIR;
    if (S_ISLNK(inode->stat.st_mode)) return -EINVAL;

    time(&inode->stat.st_mtime);

    return inode_write(inode, data, size, offset);
}

static int mfs_mknod(const char *path, mode_t mode, dev_t dev) {
    char dir_path[PATH_SIZE];
    copy_string(dir_path, path, PATH_SIZE);
    const char *basename = split_basename(dir_path);

    // Find parent directory.
    MemoryInode *dir;
    int error = find_from_path(dir_path, root, &dir);
    if (error) return error;

    // Check path availability.
    MemoryInode *child;
    error = find_child(basename, dir, &child);
    if (error != -ENOENT) return -EEXIST;

    MemoryInode *inode = new_inode();
    inode->stat.st_mode = mode;
    inode->stat.st_dev = dev;
    return inode_link(dir, inode, basename);
}

static int mfs_unlink(const char *path) {
    char dir_path[PATH_SIZE];
    copy_string(dir_path, path, PATH_SIZE);
    const char *basename = split_basename(dir_path);

    // Find parent directory.
    MemoryInode *dir;
    int error = find_from_path(dir_path, root, &dir);
    if (error) return error;

    // Find associated directory entry.
    DirEntry *entry;
    error = find_child_entry(basename, dir, &entry);
    if (error) return error;

    if (is_dir(entry->inode)) {
        return -EISDIR;
    }

    inode_unlink(entry, dir);

    return 0;
}

static int mfs_link(const char *old_path, const char *new_path) {
    MemoryInode *inode;
    int error = find_from_path(old_path, root, &inode);
    if (error) return error;

    char dir_path[PATH_SIZE];
    copy_string(dir_path, new_path, PATH_SIZE);
    const char *basename = split_basename(dir_path);

    MemoryInode *dir;
    error = find_from_path(dir_path, root, &dir);
    if (error) return error;

    MemoryInode *dest;
    error = find_child(basename, dir, &dest);
    if (error != -ENOENT) return -EEXIST;

    return inode_link(dir, inode, basename);
}

static int mfs_symlink(const char *target, const char *link_path) {
    char dir_path[PATH_SIZE];
    copy_string(dir_path, link_path, PATH_SIZE);
    const char *basename = split_basename(dir_path);

    // Find parent directory.
    MemoryInode *dir;
    int error = find_from_path(dir_path, root, &dir);
    if (error) return error;

    // Check path availability.
    MemoryInode *child;
    error = find_child(basename, dir, &child);
    if (error != -ENOENT) return -EEXIST;

    MemoryInode *inode = new_inode();
    inode->stat.st_mode = S_IFLNK | 0777;
    inode->stat.st_size = strlen(target);
    inode_update_blocks(inode);
    inode->contents.symlink = malloc(inode->stat.st_size + 1);
    strcpy(inode->contents.symlink, target);
    inode_link(dir, inode, basename);

    return 0;
}

static int mfs_readlink(const char *path, char *buffer, size_t length) {
    MemoryInode *inode;
    int error = find_from_path(path, root, &inode);
    if (error) return error;
    if (!S_ISLNK(inode->stat.st_mode)) return -EINVAL;
    size_t copy_length = inode->stat.st_size + 1;
    if (copy_length > length) {
        copy_length = length;
    }
    time(&inode->stat.st_atime);
    memcpy(buffer, inode->contents.symlink, copy_length);
    return 0;
}

static int mfs_mkdir(const char *path, mode_t mode) {
    char dir_path[PATH_SIZE];
    copy_string(dir_path, path, PATH_SIZE);
    const char *basename = split_basename(dir_path);

    // Find parent directory.
    MemoryInode *dir;
    int error = find_from_path(dir_path, root, &dir);
    if (error) return error;

    // Check path availability.
    MemoryInode *child;
    error = find_child(basename, dir, &child);
    if (error != -ENOENT) return -EEXIST;

    MemoryInode *inode = new_dir_inode();
    return inode_link(dir, inode, basename);
}

static int mfs_truncate(const char *path, off_t size, struct fuse_file_info *fi) {
    (void) fi;

    MemoryInode *inode;
    int error = find_from_path(path, root, &inode);
    if (error) return error;
    return inode_resize(inode, size);
}

static int mfs_rmdir(const char *path) {
    char dir_path[PATH_SIZE];
    copy_string(dir_path, path, PATH_SIZE);
    const char *basename = split_basename(dir_path);

    // Find parent directory.
    MemoryInode *dir;
    int error = find_from_path(dir_path, root, &dir);
    if (error) return error;

    // Find associated directory entry.
    DirEntry *entry;
    error = find_child_entry(basename, dir, &entry);
    if (error) return error;

    if (!is_dir(entry->inode)) {
        return -ENOTDIR;
    }
    if (!is_dir_empty(entry->inode)) {
        return -ENOTEMPTY;
    }

    inode_unlink(entry, dir);

    return 0;
}

static int mfs_rename(const char *old_path, const char *new_path, unsigned int flags) {
    // Decompose source path:

    char old_dir_path[PATH_SIZE];
    copy_string(old_dir_path, old_path, PATH_SIZE);
    const char *old_name = split_basename(old_dir_path);

    MemoryInode *old_dir;
    int error = find_from_path(old_dir_path, root, &old_dir);
    if (error) return error;

    DirEntry *old_entry;
    error = find_child_entry(old_name, old_dir, &old_entry);
    if (error) return error;

    // Decompose destination path:

    char new_dir_path[PATH_SIZE];
    copy_string(new_dir_path, new_path, PATH_SIZE);
    const char *new_name = split_basename(new_dir_path);

    MemoryInode *new_dir;
    error = find_from_path(new_dir_path, root, &new_dir);
    if (error) return error;

    DirEntry *new_entry;
    error = find_child_entry(new_name, new_dir, &new_entry);
    if (error != -ENOENT && (flags & RENAME_NOREPLACE)) {
        return -EEXIST;
    }
    if (error == -ENOENT && (flags & RENAME_EXCHANGE)) {
        return -EEXIST;
    }
    if (error && error != -ENOENT) return error;

    // Handle exchange request.
    if (flags & RENAME_EXCHANGE) {
        MemoryInode *existing_inode = new_entry->inode;
        new_entry->inode = old_entry->inode;
        old_entry->inode = existing_inode;
        return 0;
    }

    // Unlink destination if it already exists.
    if (new_entry) {
        if (is_dir(new_entry->inode)) {
            if (!is_dir_empty(new_entry->inode)) {
                return -ENOTEMPTY;
            }
            if (!is_dir(old_entry->inode)) {
                return -EISDIR;
            }
        } else if (is_dir(old_entry->inode)) {
            return -ENOTDIR;
        }
        inode_unlink(new_entry, new_dir);
    }

    // Move the file.
    if (old_dir == new_dir) {
        // Same directory. Just change the name.
        copy_string(old_entry->name, new_name, FILENAME_SIZE);
        return 0;
    } else {
        // Different directories. Unlink and relink.
        MemoryInode *inode = old_entry->inode;
        direntry_remove(old_entry, old_dir);
        inode_link(new_dir, inode, new_name);
    }

    return 0;
}

static struct fuse_operations mfs_operations = {
    .getattr = mfs_getattr,
    .open = mfs_open,
    .opendir = mfs_open,
    .release = mfs_release,
    .releasedir = mfs_release,
    .read = mfs_read,
    .write = mfs_write,
    .readdir = mfs_readdir,
    .mknod = mfs_mknod,
    .unlink = mfs_unlink,
    .link = mfs_link,
    .symlink = mfs_symlink,
    .readlink = mfs_readlink,
    .truncate = mfs_truncate,
    .utimens = mfs_utimens,
    .mkdir = mfs_mkdir,
    .rmdir = mfs_rmdir,
    .rename = mfs_rename,
};

/**
 * Create the root, and put our example file into the root.
 */
static void init_root() {
    root = new_dir_inode();
    MemoryInode *file = new_reg_inode();
    inode_write(file, filecontent, strlen(filecontent), 0);
    inode_link(root, file, filename);
}

int main(int argc, char *argv[])
{
    init_root();

    // The final argument is whether to launch the visualiser or not. 1 will launch visualiser, 0 will NOT launch visualiser
    return edufuse_register(argc, argv, &mfs_operations, 1);
}
