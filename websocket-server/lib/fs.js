/*
 * everarch - the hopefully ever lasting archive
 * Copyright (C) 2021-2023  Markus Peröbner
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

let fs = require('fs');
let { Observable } = require('rxjs');

function readFile(path, opts){
    return new Observable(observer => {
        fs.readFile(path, opts, (err, data) => {
            if(err){
                observer.error(err);
            } else {
                observer.next(data);
                observer.complete();
            }
        });
    });
};

module.exports = {
    readFile,
};
