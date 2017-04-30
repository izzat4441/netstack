// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <mxio/remoteio.h>
#include <mxio/socket.h>

#include <magenta/syscalls.h>

#include "apps/netstack/events.h"
#include "apps/netstack/handle_watcher.h"
#include "apps/netstack/iostate.h"
#include "apps/netstack/multiplexer.h"
#include "apps/netstack/request_queue.h"
#include "apps/netstack/socket_functions.h"
#include "apps/netstack/trace.h"

static mx_handle_t s_ctrl[2];
static mx_handle_t s_waitset;

#define START 1
#define ABORT 2

// send START command to the watcher thread
mx_status_t handle_watcher_start(void) {
  vdebug("watch_start: send START\n");
  mx_status_t r;
  uint8_t c = START;
  if ((r = mx_channel_write(s_ctrl[1], 0u, &c, 1u, NULL, 0u)) < 0) {
    error("handle_watcher_start: mx_channel_write failed (r=%d)\n", r);
    return r;
  }
  return NO_ERROR;
}

// receive a result from the watcher thread
// if the watcher is still waiting, send ABORT command first
mx_status_t handle_watcher_stop(void) {
  vdebug("watch_stop: enter\n");
  mx_status_t r;
  uint8_t c;
  mx_signals_t observed;
  if ((r = mx_object_wait_one(s_ctrl[1],
                              MX_CHANNEL_READABLE | MX_CHANNEL_PEER_CLOSED, 0u,
                              &observed)) < 0 && r != ERR_TIMED_OUT) {
    error("handle_watcher_stop: mx_object_wait_one failed (r=%d)\n", r);
    return r;
  }
  if (r == ERR_TIMED_OUT || !(observed & MX_CHANNEL_READABLE)) {
    vdebug("watch_stop: send ABORT\n");
    c = ABORT;
    if ((r = mx_channel_write(s_ctrl[1], 0u, &c, 1u, NULL, 0u)) < 0) {
      error("handle_watcher_stop: mx_channel_write failed (r=%d)\n", r);
      return r;
    }
  }

  if ((r = mx_object_wait_one(s_ctrl[1],
                              MX_CHANNEL_READABLE | MX_CHANNEL_PEER_CLOSED,
                              MX_TIME_INFINITE, &observed)) < 0) {
    error("handle_watcher_stop: mx_object_wait_one failed (r=%d)\n", r);
    return r;
  }
  if (!(observed & MX_CHANNEL_READABLE)) {
    error("handle_watcher_stop: mx_object_wait_one not readable (r=%d)\n", r);
    return ERR_BAD_STATE;
  }
  if ((r = mx_channel_read(s_ctrl[1], 0u, &c, NULL, 1u, 0u, NULL, NULL)) < 0) {
    error("handle_watcher_stop: mx_channel_read failed (r=%d)\n", r);
    return r;
  }
  vdebug("watch_stop: recv => %d (%s)\n", c, (c > 0) ? "FOUND" : "NOT FOUND");

  return (c > 0) ? 1 : 0;
}

mx_status_t handle_watcher_schedule_request(void) {
  uint32_t num_results = NSOCKETS;
  mx_waitset_result_t results[NSOCKETS];
  mx_status_t r;

  if ((r = mx_waitset_wait(s_waitset, 0u, results, &num_results)) < 0) {
    error("mx_waitset_wait failed (%d)\n", r);
    return r;
  }
  debug_socket("watcher: num_results=%d max_results=%d\n", NSOCKETS,
               num_results);

  for (int i = 0; i < (int)num_results; i++) {
    if (results[i].cookie == CTRL_COOKIE) {
      // shouldn't happen
      debug("ready_handles: skip ctrl_cookie\n");
      continue;
    }
    iostate_t* ios = (iostate_t*)results[i].cookie;
    mx_signals_t satisfied = results[i].observed;
    debug_socket("watcher: [%d] sockfd=%d, satisfied=0x%x (%s%s%s%s)\n", i,
                 ios->sockfd, satisfied,
                 (satisfied & MX_SOCKET_READABLE) ? "R" : "",
                 (satisfied & MX_SOCKET_WRITABLE) ? "W" : "",
                 (satisfied & MX_SOCKET_PEER_CLOSED) ? "C" : "",
                 (satisfied & MXSIO_SIGNAL_HALFCLOSED) ? "H" : "");

    mx_signals_t watching_signals = ios->watching_signals;
    // socket_signals_clear will change ios->watching_signals
    socket_signals_clear(ios, satisfied);

    if ((satisfied & MX_SOCKET_PEER_CLOSED) &&
        !(satisfied & MX_SOCKET_READABLE)) {
      // peer closed and no outstanding data to read
      handle_request_close(ios, satisfied);
    } else if ((satisfied & MXSIO_SIGNAL_HALFCLOSED) &&
               !(satisfied & MX_SOCKET_READABLE)) {
      // peer half closed and no outstanding data to read
      handle_request_halfclose(ios, satisfied);
    } else if (satisfied & watching_signals) {
      request_queue_t q;
      request_queue_init(&q);
      wait_queue_swap(WAIT_SOCKET, ios->sockfd, &q);

      request_t* rq;
      while ((rq = request_queue_get(&q)) != NULL) {
        handle_request(rq, EVENT_NONE, satisfied);
      }
    }
  }

  return NO_ERROR;
}

static void socket_signals_change(iostate_t* ios,
                                  mx_signals_t old_sigs,
                                  mx_signals_t new_sigs) {
  if (new_sigs)
    debug_socket("new watcing signals: ios=%p, sigs=0x%x\n", ios, new_sigs);
  else
    debug_socket("remove watching signals: ios=%p, sigs=0x%x\n", ios, old_sigs);

  mx_status_t r;
  if (old_sigs) {
    if ((r = mx_waitset_remove(s_waitset, (uint64_t)ios)) < 0) {
      error("mx_waitset_remove failed (%d)\n", r);
      return;
    }
  }
  if (new_sigs) {
    if ((r = mx_waitset_add(s_waitset, (uint64_t)ios, ios->data_h, new_sigs)) <
        0) {
      error("mx_waitset_add failed (%d)\n", r);
      return;
    }
  }
  ios->watching_signals = new_sigs;
}

void socket_signals_set(iostate_t* ios, mx_signals_t sigs) {
  debug("socket_signals_set: ios=%p, sigs==0x%x\n", ios, sigs);
  if ((ios->watching_signals & sigs) == sigs)
    return;
  sigs = ios->watching_signals | sigs;
  socket_signals_change(ios, ios->watching_signals, sigs);
}

void socket_signals_clear(iostate_t* ios, mx_signals_t sigs) {
  debug("socket_signals_clear: ios=%p, sigs=0x%x\n", ios, sigs);
  if ((ios->watching_signals & sigs) == 0x0)
    return;
  sigs = ios->watching_signals & (~sigs);
  socket_signals_change(ios, ios->watching_signals, sigs);
}

static int handle_watcher_loop(void* arg) {
  int writefd = *(int*)arg;
  free(arg);
  vdebug("handle_watcher_loop: start\n");

  mx_status_t r;

  for (;;) {
    // wait for START command (ignore ABORT received in the last round)
    mx_signals_t observed;
    if ((r = mx_object_wait_one(s_ctrl[0],
                                MX_CHANNEL_READABLE | MX_CHANNEL_PEER_CLOSED,
                                MX_TIME_INFINITE, &observed)) < 0) {
      error("handle_watcher_loop: mx_object_wait_one failed (r=%d)\n", r);
      return r;
    }
    if (!(observed & MX_CHANNEL_READABLE)) {
      error("handle_watcher_loop: mx_object_wait_one not readable (r=%d)\n", r);
      return ERR_BAD_STATE;
    }
    uint8_t c;
    if ((r = mx_channel_read(s_ctrl[0], 0u, &c, NULL, 1, 0u, NULL, NULL)) < 0) {
      error("handle_watcher_loop: mx_channel_read failed (r=%d)\n", r);
      return r;
    }
    vdebug("handle_watcher_loop: recv => %d (%s)\n", c,
           (c == START) ? "START" : (c == ABORT) ? "ABORT" : "UNKNOWN");
    if (c == ABORT) {
      continue;
    }

    // wait at most two handles
    vdebug("handle_watcher_loop: waiting\n");
    uint32_t num_results = NSOCKETS;
    mx_waitset_result_t results[NSOCKETS];
    if ((r = mx_waitset_wait(s_waitset, MX_TIME_INFINITE, results, &num_results)) < 0) {
      return r;
    }
    vdebug("handle_watcher_loop: wait_done (num=%d)\n", num_results);

    c = 0;
    for (int i = 0; i < (int)num_results; i++) {
      if (results[i].cookie != CTRL_COOKIE) {
        if (results[i].observed == 0) { // should not happen
          vdebug("handle_watcher_loop: no observed signals. skip\n");
          continue;
        }
        c = 1;
        break;
      }
    }
    vdebug("handle_watcher_loop: send %d (%s)\n", c,
           (c > 0) ? "FOUND" : "NOT FOUND");
    // if any handle except the control handle has a signal, interrupt
    // the select
    if (c > 0) {
      vdebug("handle_watcher_loop: send interrupt\n");
      if ((r = send_interrupt(writefd)) < 0) {
        error("handle_watcher_loop: send_interrupt failed (r=%d)\n", r);
        return r;
      }
    }
    // send the result
    if ((r = mx_channel_write(s_ctrl[0], 0u, &c, 1u, NULL, 0u)) < 0) {
      error("handle_watcher_loop: mx_channel_write failed (r=%d)\n", r);
      return r;
    }
  }

  return 0;
}

mx_status_t handle_watcher_init(int* readfd_) {
  mx_status_t r;
  if ((r = mx_channel_create(0, &s_ctrl[0], &s_ctrl[1])) < 0) {
    error("mx_channel_create failed (%d)\n", r);
    goto fail_channel_create;
  }
  if ((r = mx_waitset_create(0u, &s_waitset)) < 0) {
    error("mx_waitset_create failed (%d)\n", r);
    goto fail_waitset_create;
  }

  if ((r = mx_waitset_add(s_waitset, CTRL_COOKIE, s_ctrl[0],
                          MX_CHANNEL_READABLE)) < 0) {
    error("mx_waitset_add failed (%d)\n", r);
    goto fail_waitset_add;
  }

  int writefd, readfd;
  if ((r = interrupter_create(&writefd, &readfd)) < 0) {
    error("interrupter_create failed (%d)\n", r);
    goto fail_interrupter_create;
  }
  *readfd_ = readfd;

  int* arg = calloc(1, sizeof(int));
  assert(arg);
  *arg = writefd;

  thrd_t handle_watcher_thread;
  thrd_create(&handle_watcher_thread, handle_watcher_loop, arg);

  return NO_ERROR;

fail_interrupter_create:
fail_waitset_add:
  mx_handle_close(s_waitset);

fail_waitset_create:
  mx_handle_close(s_ctrl[0]);
  mx_handle_close(s_ctrl[1]);

fail_channel_create:
  return r;
}
