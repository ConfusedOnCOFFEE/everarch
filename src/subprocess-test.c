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

#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "assert.h"
#include "test.h"
#include "subprocess.h"
#include "errors.h"
#include "files.h"
#include "logger.h"

void test_cat_subprocess(){
    struct evr_subprocess sp;
    char *argv[] = {
        "/bin/cat",
        "-",
        NULL
    };
    assert(is_ok(evr_spawn(&sp, argv)));
    const char msg[] = "hello world!";
    const size_t msg_len = strlen(msg);
    struct evr_file sp_stdin;
    evr_file_bind_fd(&sp_stdin, sp.stdin);
    assert(is_ok(write_n(&sp_stdin, msg, msg_len)));
    assert(close(sp.stdin) == 0);
    char buf[msg_len + 1];
    struct evr_file sp_stdout;
    evr_file_bind_fd(&sp_stdout, sp.stdout);
    assert(is_ok(read_n(&sp_stdout, buf, msg_len, NULL, NULL)));
    buf[sizeof(buf) - 1] = '\0';
    assert(is_str_eq(msg, buf));
    assert(close(sp.stdout) == 0);
    assert(close(sp.stderr) == 0);
    int status;
    assert(waitpid(sp.pid, &status, WUNTRACED) >= 0);
    assert(status == 0);
}

void test_false_subprocess(){
    struct evr_subprocess sp;
    char *argv[] = {
        "/bin/false",
        NULL
    };
    assert(is_ok(evr_spawn(&sp, argv)));
    assert(close(sp.stdin) == 0);
    assert(close(sp.stdout) == 0);
    assert(close(sp.stderr) == 0);
    int status;
    assert(waitpid(sp.pid, &status, WUNTRACED) >= 0);
    assert(status);
}

void test_pass_path_to_subprocess(){
    struct evr_subprocess sp;
    char *argv[] = {
        "/bin/sh",
        "-c",
        "echo PATH=$PATH",
        NULL
    };
    char *my_path = evr_env_path();
    // if the following assert breaks you have to extend this test to
    // support not existing PATH environment variables
    assert(my_path);
    assert(is_ok(evr_spawn(&sp, argv)));
    assert(close(sp.stdin) == 0);
    char sp_path[4096];
    ssize_t bytes_read = read(sp.stdout, sp_path, sizeof(sp_path));
    sp_path[min(bytes_read, sizeof(sp_path)) - 1] = '\0';
    assert(strncmp(my_path, sp_path, sizeof(sp_path) - 1) == 0);
    assert(close(sp.stdout) == 0);
    assert(close(sp.stderr) == 0);
    int status;
    assert(waitpid(sp.pid, &status, WUNTRACED) >= 0);
    assert(status == 0);
}

int main(){
    evr_init_basics();
    run_test(test_cat_subprocess);
    run_test(test_false_subprocess);
    run_test(test_pass_path_to_subprocess);
    return 0;
}
