#!/usr/bin/env python3
"""Read complete Linux link-mode masks with ETHTOOL_GLINKSETTINGS; never set them."""
import argparse
import ctypes
import fcntl
import json
import socket
import struct
import sys

SIOCETHTOOL = 0x8946
ETHTOOL_GLINKSETTINGS = 0x4C
HEADER_BYTES = 48


def collect(interface):
    name = interface.encode('ascii')
    if not name or len(name) >= 16 or b'\0' in name:
        raise ValueError('Invalid interface name')
    if ctypes.sizeof(ctypes.c_void_p) != 8 or sys.byteorder != 'little':
        raise RuntimeError('This reviewed collector requires 64-bit little-endian Linux')

    def request(sock, nwords):
        data = ctypes.create_string_buffer(HEADER_BYTES + 12 * nwords)
        struct.pack_into('=I', data, 0, ETHTOOL_GLINKSETTINGS)
        struct.pack_into('b', data, 15, nwords)
        ifreq = bytearray(40)
        struct.pack_into('@16sP', ifreq, 0, name, ctypes.addressof(data))
        fcntl.ioctl(sock.fileno(), SIOCETHTOOL, ifreq, True)
        return data.raw

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        probe = request(sock, 0)
        nwords = -struct.unpack_from('b', probe, 15)[0]
        if not 1 <= nwords <= 127:
            raise RuntimeError(f'Invalid GLINKSETTINGS handshake: {nwords}')
        raw = request(sock, nwords)
    if struct.unpack_from('b', raw, 15)[0] != nwords:
        raise RuntimeError('GLINKSETTINGS word count changed')

    result = {
        'interface': interface, 'read_only': True,
        'method': 'two ETHTOOL_GLINKSETTINGS ioctl calls; no setters',
        'speed_mbps': struct.unpack_from('=I', raw, 4)[0],
        'duplex': raw[8], 'phy_address': raw[10], 'autoneg': raw[11],
        'mdix_ctrl': raw[14], 'master_slave_cfg': raw[17], 'nwords': nwords,
    }
    masks = {}
    for index, key in enumerate(('supported', 'advertising', 'lp_advertising')):
        words = struct.unpack_from('=' + 'I' * nwords, raw,
                                   HEADER_BYTES + index * nwords * 4)
        value = sum(word << (32 * n) for n, word in enumerate(words))
        masks[key] = value
        result[key] = {'words_u32_low_first': list(words), 'hex': hex(value)}
    adv = masks['advertising']
    result['advertising_named_bits'] = {
        name: bool(adv & (1 << bit))
        for name, bit in {'10baseT_Half': 0, '10baseT_Full': 1,
                          '100baseT_Half': 2, '100baseT_Full': 3,
                          '1000baseT_Half': 4, '1000baseT_Full': 5,
                          'Autoneg': 6, 'TP': 7, 'Pause': 13,
                          'Asym_Pause': 14}.items()
    }
    required = (1 << 1) | (1 << 2) | (1 << 3) | (1 << 5)
    if result['autoneg'] == 1 and adv & required == required:
        result['candidate_remove_100half_only_hex'] = hex(adv & ~(1 << 2))
        result['restore_original_advertising_hex'] = hex(adv)
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('interface', nargs='?', default='end0')
    print(json.dumps(collect(parser.parse_args().interface), indent=2))
