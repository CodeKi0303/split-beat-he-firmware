"""C86 image layout and offline packaging. Never opens or writes a device."""
import base64
import hashlib
import struct
import zlib

BOOT_BASE = 0x08000000
APP_BASE = 0x08020000
SLOT_END = 0x08060000
METADATA_BASE = 0x0805FFE0
MAX_IMAGE = METADATA_BASE - APP_BASE
IDENTITY_OFFSET = 0x400
VERSION = 0x00030029
DISPLAY_VERSION = '0.3.41'
BOOT_VERSION = 0x00030000
METADATA_MAGIC = 0x33505546
IDENTITY_MAGIC = 0x33444946


def target_role(role):
    if role not in ('left', 'right'):
        raise ValueError('Role must be left or right')
    return int(role == 'right')


def expected_identity(role, boot=False):
    return struct.pack('<8sBBHIIIII', b'C86BL003' if boot else b'C86FW003',
                       86, target_role(role), 1, BOOT_VERSION if boot else VERSION,
                       APP_BASE, MAX_IMAGE, IDENTITY_MAGIC, IDENTITY_MAGIC ^ 0xFFFFFFFF)


def validate_image(image, role, boot=False):
    limit = APP_BASE - BOOT_BASE if boot else MAX_IMAGE
    origin = BOOT_BASE if boot else APP_BASE
    if not IDENTITY_OFFSET + 32 <= len(image) <= limit:
        raise ValueError('Invalid image size')
    if not boot and len(image) % 32:
        raise ValueError('Application image must be flashword aligned')
    if image[IDENTITY_OFFSET:IDENTITY_OFFSET + 32] != expected_identity(role, boot):
        raise ValueError('Image hardware, role, layout or version identity mismatch')
    stack, reset = struct.unpack_from('<II', image)
    if not (0x20000000 < stack <= 0x20020000 and stack % 8 == 0 and
            reset & 1 and origin < reset < origin + len(image)):
        raise ValueError('Invalid image vectors')


def validate_build_info(info, image, hardware, role):
    """Reject stale build labels even when file hashes and source hashes match.

    C86's embedded identity must also agree with the authoritative package
    version. Legacy C77/C85 have no embedded update identity; their version is
    checked against the shared release constants, not inferred from bytes.
    """
    target_role(role)
    if hardware not in ('c77', 'c85', 'c86'):
        raise ValueError('Unsupported build hardware')
    if (info.get('hardware_profile') != hardware or info.get('role') != role or
            info.get('version') != DISPLAY_VERSION or
            type(info.get('version_number')) is not int or info['version_number'] != VERSION):
        raise ValueError('Build metadata hardware, role or version mismatch')
    if hardware == 'c86':
        validate_image(image, role)
        image_version = struct.unpack_from('<I', image, IDENTITY_OFFSET + 12)[0]
        if image_version != info['version_number']:
            raise ValueError('Build metadata version does not match image identity')


def metadata_record(image, role):
    validate_image(image, role)
    prefix = struct.pack('<6I', METADATA_MAGIC, len(image), zlib.crc32(image),
                         VERSION, 86 | (target_role(role) << 8) | (1 << 16), APP_BASE)
    return prefix + struct.pack('<II', zlib.crc32(prefix), METADATA_MAGIC ^ 0xFFFFFFFF)


def make_bundle(image, role):
    validate_image(image, role)
    return {'schema': 'c86-firmware-v1', 'hardware': 86, 'role': role,
            'version': DISPLAY_VERSION, 'version_number': VERSION,
            'image_base': APP_BASE, 'image_size': len(image),
            'crc32': zlib.crc32(image), 'sha256': hashlib.sha256(image).hexdigest(),
            'image_base64': base64.b64encode(image).decode('ascii')}


def validate_bundle(bundle, image, role):
    if bundle != make_bundle(image, role):
        raise ValueError('Package contents do not match the application image')


def make_factory(boot, image, role):
    validate_image(boot, role, boot=True)
    record = metadata_record(image, role)
    factory = bytearray(b'\xff' * (SLOT_END - BOOT_BASE))
    factory[:len(boot)] = boot
    offset = APP_BASE - BOOT_BASE
    factory[offset:offset + len(image)] = image
    factory[METADATA_BASE - BOOT_BASE:] = record
    return bytes(factory)


def ihex_text(segments):
    """Emit only explicitly supplied ranges; never fill reserved/settings flash."""
    lines = []
    previous_base = None
    prior_end = None

    def record(kind, address, data):
        raw = bytes([len(data)]) + address.to_bytes(2, 'big') + bytes([kind]) + data
        lines.append(':' + (raw + bytes([-sum(raw) & 255])).hex().upper())

    for origin, data in sorted(segments, key=lambda item: item[0]):
        if prior_end is not None and origin < prior_end:
            raise ValueError('Overlapping HEX image ranges')
        prior_end = origin + len(data)
        for offset in range(0, len(data), 16):
            address = origin + offset
            base = address >> 16
            if base != previous_base:
                record(4, 0, base.to_bytes(2, 'big'))
                previous_base = base
            chunk = data[offset:offset + 16]
            if (address & 0xFFFF) + len(chunk) > 0x10000:
                raise ValueError('HEX segment alignment crosses a record bank boundary')
            record(0, address & 0xFFFF, chunk)
    record(1, 0, b'')
    return '\n'.join(lines) + '\n'
