/*
 * everarch - the hopefully ever lasting archive
 * Copyright (C) 2021-2022  Markus Peröbner
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "assert.h"
#include "fs-inode.h"
#include "test.h"
#include "logger.h"

void test_create_free_inodes(){
    struct evr_fs_inode *inodes = evr_create_inodes(100);
    assert(inodes);
    struct evr_fs_inode *root = &inodes[FUSE_ROOT_ID];
    assert(root->type == evr_fs_inode_type_dir);
    assert(root->data.dir.children_len == 0);
    assert(inodes[FUSE_ROOT_ID + 1].type == evr_fs_inode_type_unlinked);
    evr_free_inodes(inodes);
}

void test_inodes_with_file(){
    size_t inodes_len = 100;
    struct evr_fs_inode *inodes = evr_create_inodes(inodes_len);
    assert(inodes);
    char *name = strdup("my-dir/file.txt");
    assert(name);
    fuse_ino_t f = evr_inode_create_file(&inodes, &inodes_len, name);
    free(name);
    assert(f != 0);
    struct evr_fs_inode *root = &inodes[FUSE_ROOT_ID];
    assert(root->data.dir.children_len == 1);
    fuse_ino_t dir = root->data.dir.children[0];
    assert(dir != 0);
    struct evr_fs_inode *dir_node = &inodes[dir];
    assert(dir_node->type == evr_fs_inode_type_dir);
    assert(is_str_eq(dir_node->name, "my-dir"));
    assert(dir_node->data.dir.children_len == 1);
    assert(dir_node->data.dir.children[0] == f);
    struct evr_fs_inode *file_node = &inodes[f];
    assert(file_node->type == evr_fs_inode_type_file);
    assert(is_str_eq(file_node->name, "file.txt"));
    evr_free_inodes(inodes);
}

int main(){
    run_test(test_create_free_inodes);
    run_test(test_inodes_with_file);
    return 0;
}
