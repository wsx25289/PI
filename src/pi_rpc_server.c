/* Copyright 2013-present Barefoot Networks, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "PI/target/pi_imp.h"
#include "PI/target/pi_tables_imp.h"
#include "PI/int/pi_int.h"
#include "PI/int/serialize.h"
#include "PI/int/rpc_common.h"

#include <nanomsg/nn.h>
#include <nanomsg/reqrep.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
  int init;
  uint32_t req_id;
  int s;
} pi_rpc_state_t;

static const char *addr = "ipc:///tmp/pi_rpc.ipc";

static pi_rpc_state_t state;

typedef struct __attribute__((packed)) {
  uint32_t id;
  uint32_t status;
} status_hdr_t;

static size_t write_status(pi_status_t status, status_hdr_t *hdr) {
  emit_uint32((char *) &hdr->id, state.req_id);
  emit_uint32((char *) &hdr->status, status);
  return sizeof(*hdr);
}

static void send_status(pi_status_t status) {
  status_hdr_t msg;
  write_status(status, &msg);
  int bytes = nn_send(state.s, &msg, sizeof(msg), 0);
  assert(bytes == sizeof(msg));
}

static void __pi_init(char *msg) {
  printf("RPC: _pi_init\n");

  (void) msg;
  send_status(_pi_init());
}

static void __pi_assign_device(char *msg) {
  printf("RPC: _pi_assign_device\n");

  pi_status_t status;
  uint32_t dev_id;
  msg += retrieve_uint32(msg, &dev_id);
  size_t p4info_size = strlen(msg) + 1;
  pi_p4info_t *p4info;
  // TODO(antonin): when is this destroyed?
  status = pi_add_config(msg, PI_CONFIG_TYPE_NATIVE_JSON, &p4info);
  if (status != PI_STATUS_SUCCESS) {
    send_status(status);
    return;
  }
  msg += p4info_size;

  // extras
  uint32_t num_extras;
  msg += retrieve_uint32(msg, &num_extras);

  size_t extras_size = sizeof(pi_assign_extra_t) * (num_extras + 1);
  pi_assign_extra_t *extras = malloc(extras_size);
  memset(extras, 0, extras_size);
  for (size_t i = 0; i < num_extras; i++) {
    extras[i].key = msg;
    msg = strchr(msg, '\0') + 1;
    extras[i].v = msg;
    msg = strchr(msg, '\0') + 1;
  }
  extras[num_extras].end_of_extras = 1;

  status = _pi_assign_device(dev_id, p4info, extras);
  free(extras);

  send_status(PI_STATUS_SUCCESS);
}

static void __pi_remove_device(char *msg) {
  printf("RPC: _pi_remove_device\n");

  uint32_t dev_id;
  retrieve_uint32(msg, &dev_id);
  send_status(_pi_remove_device(dev_id));;
}

static void __pi_destroy(char *msg) {
  printf("RPC: _pi_destroy\n");

  (void) msg;
  send_status(_pi_destroy());
}

static size_t retrieve_dev_tgt(const char *src, pi_dev_tgt_t *dev_tgt) {
  size_t s = 0;
  uint32_t tmp32;
  s += retrieve_uint32(src, &tmp32);
  dev_tgt->dev_id = tmp32;
  s += retrieve_uint32(src + s, &tmp32);
  dev_tgt->dev_pipe_mask = tmp32;
  return s;
}

static void __pi_table_entry_add(char *msg) {
  printf("RPC: _pi_table_entry_add\n");

  // TODO(antonin): find a way to take care of p4info for mk and ad
  pi_dev_tgt_t dev_tgt;
  msg += retrieve_dev_tgt(msg, &dev_tgt);
  pi_p4_id_t table_id;
  msg += retrieve_uint32(msg, &table_id);

  uint32_t mk_size;
  msg += retrieve_uint32(msg, &mk_size);
  pi_match_key_t match_key;
  match_key.p4info = NULL;  // TODO(antonin)
  match_key.table_id = table_id;
  match_key.data_size = mk_size;
  match_key.data = msg;
  msg += mk_size;

  pi_table_entry_t table_entry;
  pi_action_data_t action_data;
  table_entry.action_data = &action_data;
  table_entry.action_data->p4info = NULL;  // TODO(antonin)
  msg += retrieve_table_entry(msg, &table_entry, 0);

  uint32_t overwrite;
  msg += retrieve_uint32(msg, &overwrite);

  pi_entry_handle_t entry_handle;
  pi_status_t status = _pi_table_entry_add(dev_tgt, table_id, &match_key,
                                           &table_entry, overwrite,
                                           &entry_handle);

  typedef struct __attribute__((packed)) {
    status_hdr_t hdr;
    uint64_t h;
  } rep_t;
  rep_t rep;
  write_status(status, &rep.hdr);
  emit_uint64((char *) &rep.h, entry_handle);

  int bytes = nn_send(state.s, &rep, sizeof(rep), 0);
  assert(bytes == sizeof(rep));
}

static void __pi_table_default_action_set(char *msg) {
  printf("RPC: _pi_table_default_action_set\n");

  // TODO(antonin): find a way to take care of p4info for ad
  pi_dev_tgt_t dev_tgt;
  msg += retrieve_dev_tgt(msg, &dev_tgt);
  pi_p4_id_t table_id;
  msg += retrieve_uint32(msg, &table_id);

  pi_table_entry_t table_entry;
  pi_action_data_t action_data;
  table_entry.action_data = &action_data;
  table_entry.action_data->p4info = NULL;  // TODO(antonin)
  msg += retrieve_table_entry(msg, &table_entry, 0);

  pi_status_t status =_pi_table_default_action_set(dev_tgt, table_id,
                                                   &table_entry);
  send_status(status);
}

static void __pi_table_default_action_get(char *msg) {
  printf("RPC: _pi_table_default_action_get\n");

  uint32_t dev_id;
  msg += retrieve_uint32(msg, &dev_id);
  pi_p4_id_t table_id;
  msg += retrieve_uint32(msg, &table_id);

  pi_table_entry_t default_entry;
  pi_status_t status = _pi_table_default_action_get(dev_id, table_id,
                                                    &default_entry);

  size_t s = 0;
  s += 2 * sizeof(uint32_t);  // id and status
  s += table_entry_size(&default_entry);

  char *rep = nn_allocmsg(s, 0);
  char *rep_ = rep;
  rep_ += write_status(status, (status_hdr_t *) rep_);
  rep_ += emit_table_entry(rep_, &default_entry);

  // release target memory
  _pi_table_default_action_done(&default_entry);

  // make sure I have copied exactly the right amount
  assert((size_t) (rep_ - rep) == s);

  int bytes = nn_send(state.s, &rep, NN_MSG, 0);
  assert((size_t) bytes == s);
}

void __pi_table_entry_delete(char *msg) {
  printf("RPC: _pi_table_entry_delete\n");

  uint32_t dev_id;
  msg += retrieve_uint32(msg, &dev_id);
  pi_p4_id_t table_id;
  msg += retrieve_uint32(msg, &table_id);
  uint64_t h;
  msg += retrieve_uint64(msg, &h);

  send_status(_pi_table_entry_delete(dev_id, table_id, h));
}

static void __pi_table_entry_modify(char *msg) {
  printf("RPC: _pi_table_entry_modify\n");

  // TODO(antonin): find a way to take care of p4info for mk and ad
  uint32_t dev_id;
  msg += retrieve_uint32(msg, &dev_id);
  pi_p4_id_t table_id;
  msg += retrieve_uint32(msg, &table_id);
  uint64_t h;
  msg += retrieve_uint64(msg, &h);

  pi_table_entry_t table_entry;
  pi_action_data_t action_data;
  table_entry.action_data = &action_data;
  table_entry.action_data->p4info = NULL;  // TODO(antonin)
  msg += retrieve_table_entry(msg, &table_entry, 0);

  send_status(_pi_table_entry_modify(dev_id, table_id, h, &table_entry));
}

pi_status_t pi_rpc_server_run() {
  assert(!state.init);
  state.s = nn_socket(AF_SP, NN_REP);
  if (state.s < 0) return PI_STATUS_RPC_CONNECT_ERROR;
  if (nn_bind(state.s, addr) < 0) return PI_STATUS_RPC_CONNECT_ERROR;
  state.init = 1;

  while (1) {
    char *msg = NULL;
    int bytes = nn_recv(state.s, &msg, NN_MSG, 0);
    if (bytes < 0) return PI_STATUS_RPC_TRANSPORT_ERROR;
    if (bytes == 0) continue;

    uint32_t type;
    char *msg_ = msg;
    msg_ += retrieve_uint32(msg_, &state.req_id);
    printf("req_id: %u\n", state.req_id);
    msg_ += retrieve_uint32(msg_, &type);

    switch ((pi_rpc_msg_id_t) type) {
      case PI_RPC_INIT:
        __pi_init(msg_); break;
      case PI_RPC_ASSIGN_DEVICE:
        __pi_assign_device(msg_); break;
      case PI_RPC_REMOVE_DEVICE:
        __pi_remove_device(msg_); break;
      case PI_RPC_DESTROY:
        __pi_destroy(msg_); break;
      case PI_RPC_TABLE_ENTRY_ADD:
        __pi_table_entry_add(msg_); break;
      case PI_RPC_TABLE_DEFAULT_ACTION_SET:
        __pi_table_default_action_set(msg_); break;
      case PI_RPC_TABLE_DEFAULT_ACTION_GET:
        __pi_table_default_action_get(msg_); break;
      case PI_RPC_TABLE_ENTRY_DELETE:
        __pi_table_entry_delete(msg_); break;
      case PI_RPC_TABLE_ENTRY_MODIFY:
        __pi_table_entry_modify(msg_); break;
      default:
        assert(0);
    }

    nn_freemsg(msg);
  }

  return PI_STATUS_SUCCESS;
}

// some helper functions declared in rpc_common.h

size_t table_entry_size(const pi_table_entry_t *table_entry) {
  size_t s = 0;
  s += sizeof(uint32_t);  // action_id
  s += sizeof(uint32_t);  // action data size
  s += table_entry->action_data->data_size;
  // TODO(antonin): properties
  return s;
}

size_t emit_table_entry(char *dst, const pi_table_entry_t *table_entry) {
  size_t s = 0;
  s += emit_uint32(dst, table_entry->action_id);
  size_t ad_size = table_entry->action_data->data_size;
  s += emit_uint32(dst + s, ad_size);
  memcpy(dst + s, table_entry->action_data->data, ad_size);
  s += ad_size;
  // TODO(antonin): properties
  return s;
}

size_t retrieve_table_entry(char *src, pi_table_entry_t *table_entry,
                            int copy) {
  size_t s = 0;
  uint32_t action_id;
  s += retrieve_uint32(src, &action_id);
  table_entry->action_id = action_id;
  uint32_t ad_size;
  s += retrieve_uint32(src + s, &ad_size);

  if (copy) {
    // no alignment issue with malloc
    char *ad = malloc(sizeof(pi_action_data_t) + ad_size);
    table_entry->action_data = (pi_action_data_t *) ad;
  }

  table_entry->action_data->action_id = action_id;
  table_entry->action_data->data_size = ad_size;

  if (copy) {
    memcpy(table_entry->action_data->data, src + s, ad_size);
  } else {
    table_entry->action_data->data = src + s;
  }

  s += ad_size;

  // TODO(antonin): properties
  return s;
}
