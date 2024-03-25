// SPDX-License-Identifier: Apache-2.0
// Copyright 2022 - 2022 Pionix GmbH and Contributors to EVerest
#include <slac/slac.hpp>

#include <algorithm>
#include <cassert>
#include <cstring>

#include <arpa/inet.h>
#include <endian.h>

#include <hash_library/sha256.h>

namespace slac {
namespace utils {

// note on byte order:
//   - sha256 takes the most significant byte first from the lowest
//     memory address
//   - for the generation of the aes-128, or NMK-HS, the first octet of
//     the sha256 output is taken as the zero octet for the NMK-HS
//   - for the generation of NID, the NMK is fed into sha256, so having
//     a const char* as input should be the proper byte ordering already
void generate_nmk_hs(uint8_t nmk_hs[slac::defs::NMK_LEN], const char* plain_password, int password_len) {
    SHA256 sha256;

    // do pbkdf1 (use sha256 as hashing function, iterate 1000 times,
    // use salt)
    sha256.add(plain_password, password_len);
    sha256.add(slac::defs::NMK_HASH, sizeof(slac::defs::NMK_HASH));

    uint8_t hash[SHA256::HashBytes];
    sha256.getHash(hash);
    for (int i = 0; i < 1000 - 1; ++i) {
        sha256(hash, sizeof(hash));
        sha256.getHash(hash);
    }

    memcpy(nmk_hs, hash, slac::defs::NMK_LEN);
}

void generate_nid_from_nmk(uint8_t nid[slac::defs::NID_LEN], const uint8_t nmk[slac::defs::NMK_LEN]) {
    SHA256 sha256;

    // msb of least significant octet of NMK should be the leftmost bit
    // of the input, which corresponds to the usual const char* order

    // do pbkdf1 (use sha256 as hashing function, iterate 5 times, no
    // salt)
    uint8_t hash[SHA256::HashBytes];
    sha256(nmk, slac::defs::NMK_LEN);
    sha256.getHash(hash);
    for (int i = 0; i < 5 - 1; ++i) {
        sha256(hash, sizeof(hash));
        sha256.getHash(hash);
    }

    // use leftmost 52 bits of the hash output
    // left most bit should be bit 7 of the nid
    memcpy(nid, hash, slac::defs::NID_LEN - 1); // (bits 52 - 5)
    nid[slac::defs::NID_LEN - 1] = slac::defs::NID_SECURITY_LEVEL_SIMPLE_CONNECT |
                                   (((uint8_t)hash[6]) >> slac::defs::NID_MOST_SIGNIFANT_BYTE_SHIFT);
}

} // namespace utils

namespace messages {
void HomeplugMessage::setup_payload(void* payload, int len, uint16_t mmtype) {
    if (protocol_version == 1) {
        assert(("Homeplug Payload length too long", len < sizeof(raw_msg.v_1_1.mmentry)));

        // setup homeplug mme header
        raw_msg.v_1_1.homeplug_header.mmv = defs::MMV_HOMEPLUG_GREENPHY;
        raw_msg.v_1_1.homeplug_header.mmtype = htole16(mmtype);
        raw_msg.v_1_1.homeplug_header.fmni = 0; // not used
        raw_msg.v_1_1.homeplug_header.fmsn = 0; // not used

        // copy payload
        memcpy(raw_msg.v_1_1.mmentry, payload, len);

        // set the message size to at least MME_MIN_LENGTH
        int padding_len = defs::MME_MIN_LENGTH -
                          (sizeof(raw_msg.v_1_1.ethernet_header) + sizeof(raw_msg.v_1_1.homeplug_header) + len);
        if (padding_len > 0) {
            memset(raw_msg.v_1_1.mmentry + len, 0x00, padding_len);
        }

        raw_msg_len = std::max(sizeof(raw_msg.v_1_1.ethernet_header) + sizeof(raw_msg.v_1_1.homeplug_header) + len,
                               (size_t)defs::MME_MIN_LENGTH);
    } else if (protocol_version == 0) {
        assert(("Homeplug Payload length too long", len < sizeof(raw_msg.v_1_0.mmentry)));

        // setup homeplug mme header
        raw_msg.v_1_0.homeplug_header.mmv = defs::MMV_VENDOR_MME;
        raw_msg.v_1_0.homeplug_header.mmtype = htole16(mmtype);

        // copy payload
        memcpy(raw_msg.v_1_0.mmentry, payload, len);

        // set the message size to at least MME_MIN_LENGTH
        int padding_len = defs::MME_MIN_LENGTH -
                          (sizeof(raw_msg.v_1_0.ethernet_header) + sizeof(raw_msg.v_1_0.homeplug_header) + len);
        if (padding_len > 0) {
            memset(raw_msg.v_1_0.mmentry + len, 0x00, padding_len);
        }

        raw_msg_len = std::max(sizeof(raw_msg.v_1_0.ethernet_header) + sizeof(raw_msg.v_1_0.homeplug_header) + len,
                               (size_t)defs::MME_MIN_LENGTH);
    } else {
        throw std::out_of_range("Unsupported protocol version");
    }
}

void HomeplugMessage::setup_ethernet_header(const uint8_t dst_mac_addr[ETH_ALEN],
                                            const uint8_t src_mac_addr[ETH_ALEN]) {

    struct ether_header* ethernet_header = NULL;
    if (protocol_version == 1) {
        ethernet_header = &raw_msg.v_1_1.ethernet_header;
    } else if (protocol_version == 0) {
        ethernet_header = &raw_msg.v_1_0.ethernet_header;
    } else {
        throw std::out_of_range("Unsupported protocol version");
    }
    // ethernet frame byte order is big endian
    ethernet_header->ether_type = htons(defs::ETH_P_HOMEPLUG_GREENPHY);
    if (dst_mac_addr) {
        memcpy(ethernet_header->ether_dhost, dst_mac_addr, ETH_ALEN);
    }

    if (src_mac_addr) {
        memcpy(ethernet_header->ether_shost, src_mac_addr, ETH_ALEN);
        keep_src_mac = true;
    } else {
        keep_src_mac = false;
    }
}

uint16_t HomeplugMessage::get_mmtype() {
    if (protocol_version == 1) {
        return le16toh(raw_msg.v_1_1.homeplug_header.mmtype);
    } else if (protocol_version == 0) {
        return le16toh(raw_msg.v_1_0.homeplug_header.mmtype);
    } else {
        throw std::out_of_range("Unsupported protocol version");
    }
}

uint8_t* HomeplugMessage::get_src_mac() {
    if (protocol_version == 1) {
        return raw_msg.v_1_1.ethernet_header.ether_shost;
    } else if (protocol_version == 0) {
        return raw_msg.v_1_0.ethernet_header.ether_shost;
    } else {
        throw std::out_of_range("Unsupported protocol version");
    }
}

bool HomeplugMessage::is_valid() const {
    return raw_msg_len >= defs::MME_MIN_LENGTH;
}

} // namespace messages
} // namespace slac
