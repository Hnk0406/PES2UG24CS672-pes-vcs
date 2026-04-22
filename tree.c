// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions:     tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <n>\0<32-byte-binary-hash>"

#include "tree.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

// Forward declarations
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// Weak stub so test_tree (which doesn't link index.o) can still link.
__attribute__((weak)) int index_load(Index *index) { (void)index; return -1; }

// ─── Mode Constants ─────────────────────────────────────────────────────────

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// ─── PROVIDED ───────────────────────────────────────────────────────────────

uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;

        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);
        ptr = space + 1;

        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';
        ptr = null_byte + 1;

        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = (size_t)tree->count * 296;
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, (size_t)sorted_tree.count,
          sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        int written = sprintf((char *)buffer + offset, "%o %s",
                              entry->mode, entry->name);
        offset += (size_t)written + 1; /* +1 for the '\0' sprintf wrote */
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out  = offset;
    return 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

/*
 * write_tree_level — recursively build tree objects from a slice of index entries.
 *
 * entries : pointer array of IndexEntry*, already sorted by path
 * count   : number of entries in this slice
 * prefix  : the directory path consumed so far (e.g. "" for root, "src/" for src/)
 * id_out  : receives the ObjectID of the tree written for this level
 */
static int write_tree_level(IndexEntry **entries, int count,
                             const char *prefix, ObjectID *id_out)
{
    Tree tree;
    tree.count = 0;

    int i = 0;
    while (i < count) {
        /* path relative to this level */
        const char *rel = entries[i]->path + strlen(prefix);

        const char *slash = strchr(rel, '/');
        if (slash != NULL) {
            /* ── subdirectory entry ── */

            /* extract the immediate directory name */
            size_t dir_len = (size_t)(slash - rel);
            char dir_name[256];
            if (dir_len >= sizeof(dir_name)) return -1;
            memcpy(dir_name, rel, dir_len);
            dir_name[dir_len] = '\0';

            /* build the full prefix for this subdirectory */
            char sub_prefix[512];
            snprintf(sub_prefix, sizeof(sub_prefix), "%s%s/", prefix, dir_name);
            size_t sub_len = strlen(sub_prefix);

            /* collect all entries that belong to this subdir */
            int j = i;
            while (j < count &&
                   strncmp(entries[j]->path, sub_prefix, sub_len) == 0) {
                j++;
            }

            /* recurse */
            ObjectID subtree_id;
            if (write_tree_level(entries + i, j - i,
                                  sub_prefix, &subtree_id) != 0) return -1;

            /* add a DIR entry pointing at the subtree */
            if (tree.count >= MAX_TREE_ENTRIES) return -1;
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = MODE_DIR;
            te->hash = subtree_id;
            snprintf(te->name, sizeof(te->name), "%s", dir_name);

            i = j;
        } else {
            /* ── regular file entry ── */
            if (tree.count >= MAX_TREE_ENTRIES) return -1;
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = entries[i]->mode;
            te->hash = entries[i]->hash;
            snprintf(te->name, sizeof(te->name), "%s", rel);
            i++;
        }
    }

    /* serialize and store this tree level */
    void  *tree_data;
    size_t tree_len;
    if (tree_serialize(&tree, &tree_data, &tree_len) != 0) return -1;

    int rc = object_write(OBJ_TREE, tree_data, tree_len, id_out);
    free(tree_data);
    return rc;
}

int tree_from_index(ObjectID *id_out)
{
    /* Index is ~5 MB — allocate on heap to avoid stack overflow */
    Index *index = malloc(sizeof(Index));
    if (!index) return -1;

    if (index_load(index) != 0) { free(index); return -1; }

    if (index->count == 0) {
        free(index);
        /* empty tree */
        Tree empty;
        empty.count = 0;
        void  *data;
        size_t len;
        if (tree_serialize(&empty, &data, &len) != 0) return -1;
        int rc = object_write(OBJ_TREE, data, len, id_out);
        free(data);
        return rc;
    }

    /* build pointer array (index is already sorted by path) */
    IndexEntry *ptrs[MAX_INDEX_ENTRIES];
    for (int i = 0; i < index->count; i++)
        ptrs[i] = &index->entries[i];

    int rc = write_tree_level(ptrs, index->count, "", id_out);
    free(index);
    return rc;
}