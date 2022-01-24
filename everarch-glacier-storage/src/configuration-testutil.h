/*
 * everarch - the hopefully ever lasting archive
 * Copyright (C) 2021  Markus Peröbner
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

#ifndef __evr_configuration_testutil_h__
#define __evr_configuration_testutil_h__

#include "configuration.h"

/**
 * create_temp_evr_glacier_storage_configuration allocates a new
 * evr_glacier_storage_configuration which points to a temporary
 * glacier directory.
 *
 * Every call can assume to point to an empty glacier.
 */
evr_glacier_storage_configuration *create_temp_evr_glacier_storage_configuration();

#endif