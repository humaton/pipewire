/* PipeWire
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef __PIPEWIRE_TYPE_H__
#define __PIPEWIRE_TYPE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/type-map.h>
#include <spa/event-node.h>
#include <spa/command-node.h>
#include <spa/monitor.h>
#include <spa/param-alloc.h>

#include <pipewire/client/map.h>
#include <pipewire/client/transport.h>

struct pw_interface {
  uint32_t    n_methods;
  const void *methods;
  uint32_t    n_events;
  const void *events;
};

/**
 * pw_type:
 *
 * PipeWire Type support struct.
 */
struct pw_type {
  SpaTypeMap *map;

  SpaType core;
  SpaType registry;
  SpaType node;
  SpaType node_factory;
  SpaType link;
  SpaType client;
  SpaType client_node;
  SpaType module;

  SpaType spa_node;
  SpaType spa_clock;
  SpaType spa_monitor;
  SpaType spa_format;
  SpaType spa_props;

  SpaTypeMeta meta;
  SpaTypeData data;
  SpaTypeEventNode event_node;
  SpaTypeCommandNode command_node;
  SpaTypeMonitor monitor;
  SpaTypeParamAllocBuffers param_alloc_buffers;
  SpaTypeParamAllocMetaEnable param_alloc_meta_enable;
  SpaTypeParamAllocVideoPadding param_alloc_video_padding;
  struct pw_type_event_transport event_transport;
};

void pw_type_init (struct pw_type *type);

bool pw_pod_remap_data  (uint32_t type, void *body, uint32_t size, struct pw_map *types);

static inline bool
pw_pod_remap (SpaPOD *pod, struct pw_map *types)
{
  return pw_pod_remap_data (pod->type, SPA_POD_BODY (pod), pod->size, types);
}

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_TYPE_H__ */