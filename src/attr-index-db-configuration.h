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

#ifndef __attr_index_db_configuration_h__
#define __attr_index_db_configuration_h__

#include "config.h"

struct evr_attr_index_db_configuration {
    char *state_dir_path;
};

struct evr_attr_index_db_configuration *evr_create_attr_index_db_configuration();

void evr_free_attr_index_db_configuration(struct evr_attr_index_db_configuration *config);

int evr_merge_attr_index_db_configuration(void *config, const char *config_path);

int evr_expand_attr_index_db_configuration(void *config);

#endif