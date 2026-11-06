import struct
import sys
import pefile


def collect(entry):
    data = getattr(entry, "data", None)
    if data is not None:
        yield entry
        return
    directory = getattr(entry, "directory", None)
    if directory is not None:
        for child in directory.entries:
            yield from collect(child)


def first_data(entry):
    for leaf in collect(entry):
        return leaf.data.struct
    return None


def extract_icons(exe, out):
    pe = pefile.PE(exe)
    icons = {}
    group = None
    if not hasattr(pe, "DIRECTORY_ENTRY_RESOURCE"):
        raise SystemExit("no resources")
    for entry in pe.DIRECTORY_ENTRY_RESOURCE.entries:
        if entry.id == 3:
            for item in entry.directory.entries:
                data = first_data(item)
                if data is not None:
                    icons[item.id] = pe.get_data(data.OffsetToData, data.Size)
        elif entry.id == 14:
            for item in entry.directory.entries:
                data = first_data(item)
                if data is not None:
                    group = pe.get_data(data.OffsetToData, data.Size)
    if not group:
        raise SystemExit("no RT_GROUP_ICON")
    _, _, count = struct.unpack_from("<HHH", group, 0)
    entries = []
    offset = 6
    for _ in range(count):
        width, height, colors, reserved, planes, bpp, size, nid = struct.unpack_from("<BBBBHHIH", group, offset)
        offset += 14
        entries.append((width, height, colors, reserved, planes, bpp, size, nid))
    header = struct.pack("<HHH", 0, 1, count)
    directory = b""
    blobs = b""
    data_offset = 6 + 16 * count
    for width, height, colors, reserved, planes, bpp, size, nid in entries:
        image = icons[nid]
        directory += struct.pack("<BBBBHHII", width, height, colors, reserved,
                                 planes, bpp, len(image), data_offset + len(blobs))
        blobs += image
    with open(out, "wb") as f:
        f.write(header + directory + blobs)
    print("extracted", count, "sizes:",
          ", ".join(f"{w or 256}x{h or 256}" for w, h, *_ in entries))


if __name__ == "__main__":
    extract_icons(sys.argv[1], sys.argv[2])
