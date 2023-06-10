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

let childProcess = require('child_process');
let readline = require('readline');
let { EMPTY, merge, Observable, of } = require('rxjs');
let { catchError, concatWith, ignoreElements, map, mergeMap, share, switchMap, takeUntil, tap, toArray } = require('rxjs/operators');
let ws = require('ws');

let { readConfig } = require('./config');

let nextConnectionId = 1;

readConfig(process.argv[2])
    .pipe(
        switchMap(config => new Observable(observer => {
            let server = new ws.Server({
                port: config.port,
            });
            console.log(`Listening on ws://localhost:${config.port}`);
            server.on('connection', handle);
            function handle(socket, request){
                observer.next({
                    socket,
                    request,
                });
            }
            return () => {
                server.close();
                server.off('connection', handle);
            };
        })
                  .pipe(
                      mergeMap(connection => {
                          let connectionId = nextConnectionId++;
                          return handleSocket(config, connection, connectionId)
                              .pipe(
                                  catchError(error => {
                                      console.log(`${connectionTag(connectionId)} crashed:`, error);
                                      return EMPTY;
                                  }),
                              );
                      }),
                  )),
    )
    .subscribe();

function handleSocket(config, connection, connectionId){
    let { socket, request } = connection;
    let address = getClientAddress(request);
    log(`Connection from ${address}`);
    let authenticatedUser = null;
    return new Observable(observer => {
        socket.on('close', handleClose);
        socket.on('message', handleMsg);
        function handleMsg(msg){
            observer.next(msg);
        }
        function handleClose(){
            log('Socket closed');
            observer.complete();
        }
        return () => {
            socket.off('message', handleMsg);
            socket.off('close', handleClose);
            socket.close();
        };
    })
        .pipe(
            share(),
            map(msg => JSON.parse(msg)),
            observable => {
                let complete = observable
                    .pipe(
                        switchMap(() => EMPTY),
                        toArray(),
                    );
                return observable
                    .pipe(
                        mergeMap(msg => {
                            switch(msg.cmd){
                            default:
                                log('Retrieved unknown command:', JSON.stringify(msg));
                                send({
                                    ch: msg.ch,
                                    status: 'unknown',
                                });
                                break;
                            case 'auth':
                                if(msg.type === 'basic'){
                                    let user = config.user.hasOwnProperty(msg.user) && config.user[msg.user];
                                    if(user && user.password === msg.password){
                                        log(`User ${msg.user} connected`);
                                        authenticatedUser = user;
                                        send({
                                            ch: msg.ch,
                                            status: 'authenticated'
                                        });
                                        break;
                                    }
                                }
                                authenticatedUser = null;
                                sendUnauthenticated(msg.ch);
                                socket.close();
                                break;
                            case 'watch':
                                if(!authenticatedUser){
                                    sendUnauthenticated(msg.ch);
                                    break;
                                }
                                return watch(msg.lastModifiedAfter || 0, msg.flags)
                                    .pipe(
                                        buildModifiedClaimSetFilter(msg.filter),
                                        sendErrorOnError(msg),
                                        tap(modifiedClaimSet => {
                                            send({
                                                ch: msg.ch,
                                                status: 'blob-modified',
                                                ...modifiedClaimSet,
                                            });
                                        }),
                                    );
                            case 'get-verify':
                                if(!authenticatedUser){
                                    sendUnauthenticated(msg.ch);
                                    break;
                                }
                                return getAndVerify(msg.ref)
                                    .pipe(
                                        sendErrorOnError(msg),
                                        tap(content => {
                                            send({
                                                ch: msg.ch,
                                                status: 'get',
                                                body: content.toString(),
                                            });
                                        }),
                                    );
                            case 'sign-put':
                                if(!authenticatedUser){
                                    sendUnauthenticated(msg.ch);
                                    break;
                                }
                                return signAndPut(msg.body)
                                    .pipe(
                                        sendErrorOnError(msg),
                                        tap(ref => {
                                            send({
                                                ch: msg.ch,
                                                status: 'put',
                                                ref,
                                            });
                                        }),
                                    );
                            }
                            return EMPTY;
                        }),
                        takeUntil(complete),
                    );
            },
        );

    function sendUnauthenticated(ch){
        send({
            ch,
            status: 'unauthenticated',
        });
    }

    function sendErrorOnError(msg){
        return catchError(err => {
            if(err instanceof ChildProcessError){
                if(err.exitCode !== 2 && err.exitCode !== 5){
                    log(`Child process failed with exit code ${err.exitCode}: ${err.stderr}`);
                }
                send({
                    ch: msg.ch,
                    status: 'error',
                    errorCode: err.exitCode,
                });
            } else {
                log('Error while processing command:', err);
                send({
                    ch: msg.ch,
                    status: 'error',
                    errorCode: 1,
                });
            }
            return EMPTY;
        });
    }

    function send(msg){
        socket.send(JSON.stringify(msg));
    }

    function log(){
        let msg = [
            connectionTag(connectionId),
            ...arguments,
        ];
        console.log(...msg);
    }
}

function connectionTag(connectionId){
    return `[C${connectionId.toString(16)}]`;
}

function getClientAddress(request){
    let forwardedFor = request.headers['x-forwarded-for'];
    if(forwardedFor){
        return forwardedFor;
    }
    return request.socket.remoteAddress;
}

function watch(lastModifiedAfter=0, flags=undefined){
    let args = [
        'watch',
        `--last-modified-after=${lastModifiedAfter}`,
    ];
    if(flags){
        args.push(`--flags=${flags}`);
    }
    return evr(args)
        .pipe(
            switchMap(proc => new Observable(observer => {
                let rl = readline.createInterface({
                    input: proc.stdout,
                    terminal: false
                });
                rl.on('close', handleClose);
                rl.on('line', handleLine);
                function handleClose(){
                    observer.complete();
                }
                function handleLine(line){
                    observer.next(line);
                }
                return () => {
                    rl.off('line', handleLine);
                    rl.off('close', handleClose);
                    rl.close();
                };
            })),
            map(line => {
                let [ref, lastModified, sentinel] = line.split(' ');
                return {
                    ref,
                    lastModified: parseInt(lastModified),
                };
            }),
        );
}

function buildModifiedClaimSetFilter(filterDesc){
    if(!filterDesc){
        return tap(() => {});
    }
    if(filterDesc.type !== 'namespace'){
        throw new Error(`Unknown filter type ${filterDesc.type}`);
    }
    if(!filterDesc.ns){
        throw new Error(`namespace filter requires ns property with namespace`);
    }
    return mergeMap(modifiedClaimSet => getAndVerify(modifiedClaimSet.ref).pipe(
        switchMap(claimSet => {
            if(claimSet.indexOf(filterDesc.ns) !== -1){
                return of(modifiedClaimSet);
            }
            return EMPTY;
        }),
    ), undefined, 1);
}

function getAndVerify(ref){
    return evr(['get-verify', ref])
        .pipe(
            readProcessStdout(),
            concatBuffers(),
        );
}

function signAndPut(claimSet){
    return evr(['sign-put', '--flags=1'])
        .pipe(
            switchMap(proc => merge(
                of(proc).pipe(readProcessStdout()),
                of(undefined).pipe(switchMap(() => {
                    proc.stdin.end(claimSet);
                    return EMPTY;
                })),
            )),
            concatBuffers(),
            map(stdout => stdout.toString().trim()),
        );
}

function readProcessStdout(){
    return switchMap(proc => new Observable(observer => {
        proc.stdout.on('data', handleData);
        proc.stdout.on('end', handleEnd);
        function handleData(data){
            observer.next(data);
        }
        function handleEnd(){
            observer.complete();
        }
        return () => {
            proc.stdout.off('data', handleData);
            proc.stdout.off('end', handleEnd);
        };
    }));
}

function concatBuffers(){
    return observable => {
        let buffers = [];
        return observable
            .pipe(
                tap(buf => buffers.push(buf)),
                ignoreElements(),
                concatWith(new Observable(observer => {
                    observer.next(Buffer.concat(buffers));
                    observer.complete();
                })),
            );
    };
}

function evr(args){
    return spawn('evr', args);
}

class ChildProcessError extends Error{
    constructor(exitCode, stderr){
        super(`Child process failed with exit code ${exitCode}: ${stderr}`);
        this.exitCode = exitCode;
        this.stderr = stderr;
    }
}

function spawn(cmd, args=[]){
    return new Observable(observer => {
        let proc = childProcess.spawn(cmd, args);
        let errout = [];
        proc.stderr.on('data', handleData);
        function handleData(data){
            if(errout.length > 20){
                errout.splice(0, errout.length - 20);
            }
            errout.push(data);
        }
        proc.on('close', exitCode => {
            if(exitCode){
                observer.error(new ChildProcessError(exitCode, errout.join('')));
            } else {
                observer.complete();
            }
        });
        observer.next(proc);
        return () => {
            proc.stderr.off('data', handleData);
            proc.kill();
        };
    });
}

