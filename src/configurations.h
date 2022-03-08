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

#ifndef __configurations_h__
#define __configurations_h__

#include "config.h"

#include <string.h>
#include <cjson/cJSON.h>

#include "dyn-mem.h"

#define replace_string(dest, src, error_target) \
    do {                                        \
        char *src_var = src;                    \
        if(src_var){                            \
            if(dest){free(dest);}               \
            size_t src_len = strlen(src_var);   \
            dest = malloc(src_len+1);           \
            if(!dest){goto error_target;}       \
            memcpy(dest, src_var, src_len+1);   \
        }                                       \
    } while(0)

#define evr_single_expand_property(p, fail)     \
    if(evr_single_wordexp(&p) != evr_ok) {      \
        goto fail;                              \
    }

int evr_single_wordexp(char **pathname);

cJSON *evr_parse_json_config(const char *path);

/**
 * evr_load_configurations walks over a list of to be expanded paths
 * and merges the config behind that path into a root config.
 */
int evr_load_configurations(void *config, const char **paths, size_t paths_len, int (*merge)(void *config, const char *config_path), int (*expand)(void *config));


char *evr_get_object_string_property(const cJSON *obj, const char* key);

#endif
