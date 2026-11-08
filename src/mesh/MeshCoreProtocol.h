// SPDX-License-Identifier: AGPL-3.0-only
//
// MeshCore wire constants shared by the repeater and its firewall.
#pragma once

// MeshCore Dispatcher::tryParsePacket payload types (see MeshCore src/Packet.h)
#define MC_PAYLOAD_TYPE_REQ 0x00
#define MC_PAYLOAD_TYPE_RESPONSE 0x01
#define MC_PAYLOAD_TYPE_TXT_MSG 0x02
#define MC_PAYLOAD_TYPE_ACK 0x03
#define MC_PAYLOAD_TYPE_ADVERT 0x04
#define MC_PAYLOAD_TYPE_GRP_TXT 0x05
#define MC_PAYLOAD_TYPE_GRP_DATA 0x06
#define MC_PAYLOAD_TYPE_ANON_REQ 0x07
#define MC_PAYLOAD_TYPE_PATH 0x08
#define MC_PAYLOAD_TYPE_TRACE 0x09
#define MC_PAYLOAD_TYPE_MULTIPART 0x0A
#define MC_PAYLOAD_TYPE_CONTROL 0x0B
#define MC_PAYLOAD_TYPE_RAW_CUSTOM 0x0F

// MeshCore route types (Packet.h PH_ROUTE_MASK)
#define MC_ROUTE_TRANSPORT_FLOOD 0x00
#define MC_ROUTE_FLOOD 0x01
#define MC_ROUTE_DIRECT 0x02
#define MC_ROUTE_TRANSPORT_DIRECT 0x03

// MeshCore path_len byte (Packet.h): upper 2 bits = hash_size-1, lower 6 = count. Mode 3 reserved.
#define MC_PATH_MODE_MASK 0xC0
#define MC_PATH_MODE_RESERVED 0xC0
#define MC_PATH_COUNT_MASK 0x3F
#define MC_PATH_SIZE_SHIFT 6

// MeshCore header byte 0: bits 6-7 are the payload version (must be PAYLOAD_VER_1 = 0).
#define MC_PAYLOAD_VER_SHIFT 6

// A full Meshtastic on-air header (to/from/id/flags/channel/next_hop/relay_node) must fit for a
// frame to be Meshtastic at all; spelled out to keep this file free of RadioInterface.h.
#define MC_MESHTASTIC_HEADER_LEN 16

// MeshCore ADVERT encoding (MeshCore.h / AdvertDataHelpers.h)
#define MC_PUB_KEY_SIZE 32
#define MC_SIGNATURE_SIZE 64
#define MAX_ADVERT_DATA_SIZE 32
#define MC_ADV_TYPE_REPEATER 0x02
#define MC_ADV_NAME_MASK 0x80
#define MC_MAX_PACKET_PAYLOAD 184 // MeshCore.h MAX_PACKET_PAYLOAD

// MeshCore CONTROL subtypes (simple_repeater MyMesh.cpp)
#define MC_CTL_DISCOVER_REQ 0x80
#define MC_CTL_DISCOVER_RESP 0x90
